// SPDX-License-Identifier: BSD-2-Clause-Patent
// Driver HID digitizer (10 dedos + gestos de borda) para NT36672A no SM6125 (IOHIDFamily 2031.120.4).
#include "OriginalHidDeviceABI.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSString.h>
#include <string.h>

extern "C" {
volatile uint32_t sm6125_touch_seq = 0;
volatile uint32_t sm6125_touch_count = 0;
volatile uint32_t sm6125_touch_hid_reports = 0;
volatile uint32_t sm6125_touch_max_x = 1080u;
volatile uint32_t sm6125_touch_max_y = 2340u;
volatile int sm6125_touch_xs[12] = {};
volatile int sm6125_touch_ys[12] = {};
volatile int sm6125_touch_ids[12] = {};
volatile uint32_t sm6125_hid_diag[8] = {}; /* [0]=start,[1]=drv,[2]=notify,[3]=ret,[4]=edge_calls,[5]=edge_dig,[6]=open_flags,[7]=usage */

void sm6125_touch_set_sink(void (*fn)(void *), void *ctx);
void sm6125_buttons_set_sink(void (*fn)(void *, uint32_t), void *ctx);
}

static void (*s_touch_sink)(void *) = nullptr;
static void *s_touch_sink_ctx = nullptr;
static void (*s_buttons_sink)(void *, uint32_t) = nullptr;
static void *s_buttons_sink_ctx = nullptr;

extern "C" void sm6125_touch_set_sink(void (*fn)(void *), void *ctx)
{
  s_touch_sink = fn;
  __asm__ volatile("dmb ish" : : : "memory");
  s_touch_sink_ctx = ctx;
}

extern "C" void sm6125_touch_notify_sink(void)
{
  void (*fn)(void *) = s_touch_sink;
  void *ctx = s_touch_sink_ctx;
  if (fn && ctx) {
    fn(ctx);
  }
}

extern "C" void sm6125_buttons_set_sink(void (*fn)(void *, uint32_t), void *ctx)
{
  s_buttons_sink = fn;
  __asm__ volatile("dmb ish" : : : "memory");
  s_buttons_sink_ctx = ctx;
}

extern "C" void sm6125_buttons_notify_sink(uint32_t mask)
{
  void (*fn)(void *, uint32_t) = s_buttons_sink;
  void *ctx = s_buttons_sink_ctx;
  if (fn && ctx) {
    fn(ctx, mask);
  }
}

#define TOUCH_FINGERS 10

#define FINGER_DESC \
  0x09, 0x22, 0xa1, 0x02, \
  0x09, 0x42, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02, \
  0x09, 0x47, 0x81, 0x02, \
  0x75, 0x06, 0x95, 0x01, 0x81, 0x03, \
  0x09, 0x51, 0x75, 0x08, 0x95, 0x01, 0x15, 0x00, 0x26, 0xff, 0x00, 0x81, 0x02, \
  0x05, 0x01, 0x09, 0x30, 0x15, 0x00, 0x26, 0xff, 0x7f, 0x75, 0x10, 0x95, 0x01, 0x81, 0x02, \
  0x09, 0x31, 0x81, 0x02, \
  0x05, 0x0d, 0xc0,

static const unsigned char kKeyboardConsumerDescriptor[] = {
  0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01,
  0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
  0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
  0x75, 0x08, 0x95, 0x01, 0x81, 0x03,
  0x19, 0x00, 0x29, 0x65, 0x15, 0x00, 0x25, 0x65,
  0x75, 0x08, 0x95, 0x06, 0x81, 0x00,
  0x85, 0x02, 0x05, 0x0c, 0x09, 0x40, 0x09, 0x30, 0x09, 0xe9, 0x09, 0xea, 0x15, 0x00, 0x25, 0x01,
  0x75, 0x01, 0x95, 0x04, 0x81, 0x02,
  0x75, 0x04, 0x95, 0x01, 0x81, 0x03, 0xc0
};

static const unsigned char kTouchDescriptor[] = {
  0x05, 0x0d, 0x09, 0x04, 0xa1, 0x01, 0x85, 0x03,
  FINGER_DESC FINGER_DESC FINGER_DESC FINGER_DESC FINGER_DESC
  FINGER_DESC FINGER_DESC FINGER_DESC FINGER_DESC FINGER_DESC
  0x09, 0x54, 0x75, 0x08, 0x95, 0x01, 0x15, 0x00, 0x26, 0xff, 0x00, 0x81, 0x02, 0xc0
};

namespace edge {
enum {
  kSlots = 256,
  kDispatch = 198,
  kGetType = 18,
  kGetInteger = 23,
  kSetInteger = 24,
  kGetFixed = 25,
  kContacts = 256
};
enum : uint32_t {
  kDigitizer = 11,
  kX = 0xB0000,
  kY = 0xB0001,
  kIndex = 0xB0005,
  kMask = 0xB0007,
  kTouch = 0xB0009,
  kMaskTouch = 2,
  kFromEdgeTip = 0x800
};

/* ~48 px da borda em painel 1080x2340 (~3 mm) em coordenadas IOFixed 0..65536. */
static const int32_t kEdgeX = 48 * 65536 / 1080;
static const int32_t kEdgeY = 48 * 65536 / 2340;
static uintptr_t originalDispatch = 0;
static bool fromEdge[kContacts] = {};

template <typename R, typename... A>
static R call(void *object, unsigned slot, A... args)
{
  return reinterpret_cast<R (*)(void *, A...)>((*reinterpret_cast<uintptr_t **>(object))[slot])(object, args...);
}

static bool mark(void *child)
{
  if (call<uint32_t, IOOptionBits>(child, kGetType, 0) != kDigitizer) {
    return false;
  }
  const uint32_t index = static_cast<uint32_t>(call<int32_t, uint32_t, IOOptionBits>(child, kGetInteger, kIndex, 0));
  if (index >= kContacts) {
    return false;
  }
  const uint32_t mask = static_cast<uint32_t>(call<int32_t, uint32_t, IOOptionBits>(child, kGetInteger, kMask, 0));
  const bool touch = call<int32_t, uint32_t, IOOptionBits>(child, kGetInteger, kTouch, 0) != 0;
  if ((mask & kMaskTouch) && touch) {
    const int32_t x = call<int32_t, uint32_t, IOOptionBits>(child, kGetFixed, kX, 0);
    const int32_t y = call<int32_t, uint32_t, IOOptionBits>(child, kGetFixed, kY, 0);
    fromEdge[index] = (x < kEdgeX) || (x > 65536 - kEdgeX) || (y < kEdgeY) || (y > 65536 - kEdgeY);
  }
  const bool edged = fromEdge[index];
  if (edged) {
    call<void, uint32_t, int32_t, IOOptionBits>(child, kSetInteger, kMask, static_cast<int32_t>(mask | kFromEdgeTip), 0);
  }
  if (!touch && (mask & kMaskTouch)) {
    fromEdge[index] = false;
  }
  return edged;
}

static void dispatch(IOService *self, OSObject *event, IOOptionBits options)
{
  sm6125_hid_diag[4] = sm6125_hid_diag[4] + 1u;
  if (event && call<uint32_t, IOOptionBits>(event, kGetType, 0) == kDigitizer) {
    sm6125_hid_diag[5] = sm6125_hid_diag[5] + 1u;
    OSArray *children = *reinterpret_cast<OSArray **>(reinterpret_cast<uint8_t *>(event) + 24);
    if (children && OSDynamicCast(OSArray, children) && children->getCount()) {
      bool any = false;
      for (unsigned i = 0; i < children->getCount(); ++i) {
        OSObject *child = children->getObject(i);
        if (child && child->metaCast("IOHIDEvent")) {
          any |= mark(child);
        }
      }
      if (any) {
        const uint32_t mask = static_cast<uint32_t>(call<int32_t, uint32_t, IOOptionBits>(event, kGetInteger, kMask, 0));
        call<void, uint32_t, int32_t, IOOptionBits>(event, kSetInteger, kMask, static_cast<int32_t>(mask | kFromEdgeTip), 0);
      }
    } else if (!children) {
      mark(event);
    }
  }
  reinterpret_cast<void (*)(IOService *, OSObject *, IOOptionBits)>(originalDispatch)(self, event, options);
}

static void install(IORegistryEntry *driver)
{
  if (originalDispatch || !driver) {
    return;
  }
  uintptr_t *table = *reinterpret_cast<uintptr_t **>(driver);
  uintptr_t *copy = static_cast<uintptr_t *>(IOMalloc(kSlots * sizeof(uintptr_t)));
  if (!copy) {
    return;
  }
  memcpy(copy, table, kSlots * sizeof(uintptr_t));
  originalDispatch = copy[kDispatch];
  copy[kDispatch] = reinterpret_cast<uintptr_t>(&dispatch);
  __asm__ volatile("dmb ish" : : : "memory");
  *reinterpret_cast<uintptr_t **>(driver) = copy;
}
}  // namespace edge

class Sm6125HidInput : public IOHIDDevice {
  OSDeclareDefaultStructors(Sm6125HidInput)
  IOWorkLoop *inputLoop = nullptr;
  IOTimerEventSource *timer = nullptr;
  bool eventDriverFound = false;
  uint32_t lastSeq = 0;
  int prevIds[TOUCH_FINGERS] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
  int prevX[TOUCH_FINGERS] = {};
  int prevY[TOUCH_FINGERS] = {};

  static unsigned transformCoordinate(int value, uint32_t maximum)
  {
    int max_i = (maximum > 1u) ? static_cast<int>(maximum) : 1080;
    if (value < 0) value = 0;
    if (value >= max_i) value = max_i - 1;
    return static_cast<unsigned>((static_cast<long long>(value) * 32767LL) / (max_i - 1));
  }

  static void touchNotify(void *context)
  {
    sm6125_hid_diag[2] = sm6125_hid_diag[2] + 1u;
    Sm6125HidInput *self = static_cast<Sm6125HidInput *>(context);
    if (self && self->inputLoop) {
      self->inputLoop->runAction(&Sm6125HidInput::touchAction, self);
    }
  }

  static IOReturn touchAction(OSObject *owner, void *, void *, void *, void *)
  {
    Sm6125HidInput *self = OSDynamicCast(Sm6125HidInput, owner);
    if (!self) {
      return kIOReturnSuccess;
    }
    self->touchSend();
    return kIOReturnSuccess;
  }

  static void buttonsNotify(void *context, uint32_t mask)
  {
    Sm6125HidInput *self = static_cast<Sm6125HidInput *>(context);
    if (self && self->inputLoop) {
      self->inputLoop->runAction(&Sm6125HidInput::buttonsAction, self,
                                 reinterpret_cast<void *>(static_cast<uintptr_t>(mask)));
    }
  }

  static IOReturn buttonsAction(OSObject *owner, void *mask_arg, void *, void *, void *)
  {
    Sm6125HidInput *self = OSDynamicCast(Sm6125HidInput, owner);
    if (!self || !self->hasEventDriverCached()) {
      return kIOReturnSuccess;
    }
    uint8_t report[2] = {2u, static_cast<uint8_t>(reinterpret_cast<uintptr_t>(mask_arg) & 0x0Fu)};
    IOBufferMemoryDescriptor *buffer = IOBufferMemoryDescriptor::withBytes(report, sizeof(report), kIODirectionNone);
    if (buffer) {
      (void)self->handleReport(buffer, 0, 0);
      OSSafeReleaseNULL(buffer);
    }
    return kIOReturnSuccess;
  }

  void touchSend()
  {
    if (!hasEventDriverCached()) {
      return;
    }
    uint32_t before = sm6125_touch_seq;
    if ((before & 1u) || before == lastSeq) {
      return;
    }
    __asm__ volatile("dmb ish" : : : "memory");
    unsigned count = sm6125_touch_count;
    if (count > TOUCH_FINGERS) count = TOUCH_FINGERS;
    int ids[TOUCH_FINGERS], xs[TOUCH_FINGERS], ys[TOUCH_FINGERS];
    for (unsigned i = 0; i < count; ++i) {
      ids[i] = sm6125_touch_ids[i];
      xs[i] = sm6125_touch_xs[i];
      ys[i] = sm6125_touch_ys[i];
    }
    uint32_t max_x = sm6125_touch_max_x;
    uint32_t max_y = sm6125_touch_max_y;
    __asm__ volatile("dmb ish" : : : "memory");
    if (sm6125_touch_seq != before) {
      return;
    }
    lastSeq = before;

    unsigned char report[1 + TOUCH_FINGERS * 6 + 1] = {3};
    unsigned slot = 0;
    int nextIds[TOUCH_FINGERS], nextX[TOUCH_FINGERS], nextY[TOUCH_FINGERS];
    for (unsigned i = 0; i < count && slot < TOUCH_FINGERS; ++i, ++slot) {
      unsigned x = transformCoordinate(xs[i], max_x);
      unsigned y = transformCoordinate(ys[i], max_y);
      unsigned char *f = report + 1 + slot * 6;
      f[0] = 3;
      f[1] = static_cast<unsigned char>(ids[i] & 0xFF);
      f[2] = static_cast<unsigned char>(x & 0xFF);
      f[3] = static_cast<unsigned char>((x >> 8) & 0xFF);
      f[4] = static_cast<unsigned char>(y & 0xFF);
      f[5] = static_cast<unsigned char>((y >> 8) & 0xFF);
      nextIds[slot] = ids[i];
      nextX[slot] = static_cast<int>(x);
      nextY[slot] = static_cast<int>(y);
    }
    for (unsigned i = 0; i < TOUCH_FINGERS && slot < TOUCH_FINGERS; ++i) {
      if (prevIds[i] < 0) continue;
      bool present = false;
      for (unsigned j = 0; j < count; ++j) {
        if (ids[j] == prevIds[i]) {
          present = true;
          break;
        }
      }
      if (!present) {
        unsigned char *f = report + 1 + slot * 6;
        f[0] = 2; /* Tip=0, TouchValid=1 */
        f[1] = static_cast<unsigned char>(prevIds[i] & 0xFF);
        f[2] = static_cast<unsigned char>(prevX[i] & 0xFF);
        f[3] = static_cast<unsigned char>((prevX[i] >> 8) & 0xFF);
        f[4] = static_cast<unsigned char>(prevY[i] & 0xFF);
        f[5] = static_cast<unsigned char>((prevY[i] >> 8) & 0xFF);
        ++slot;
      }
    }
    for (unsigned i = 0; i < TOUCH_FINGERS; ++i) {
      prevIds[i] = (i < count) ? nextIds[i] : -1;
      prevX[i] = (i < count) ? nextX[i] : 0;
      prevY[i] = (i < count) ? nextY[i] : 0;
    }
    if (!slot) {
      return;
    }
    report[1 + TOUCH_FINGERS * 6] = static_cast<unsigned char>(slot);
    sm6125_touch_hid_reports = sm6125_touch_hid_reports + 1u;

    IOBufferMemoryDescriptor *buffer = IOBufferMemoryDescriptor::withBytes(report, sizeof(report), kIODirectionNone);
    if (buffer) {
      IOReturn hr = handleReport(buffer, 0, 0);
      sm6125_hid_diag[3] = static_cast<uint32_t>(hr);
      OSSafeReleaseNULL(buffer);
    }
  }

  bool hasEventDriverCached()
  {
    IORegistryEntry *driver = eventDriver(this, 4);
    if (!eventDriverFound && driver) {
      eventDriverFound = true;
      sm6125_hid_diag[1] = 1u;
      edge::install(driver);
    }
    if (driver) {
      IOService *drvSvc = OSDynamicCast(IOService, driver);
      uint32_t flags = (this->isOpen() ? 1u : 0u) | ((drvSvc && drvSvc->isOpen()) ? 2u : 0u);
      OSIterator *it = drvSvc ? drvSvc->getClientIterator() : nullptr;
      uint32_t clients = 0u;
      if (it) {
        while (it->getNextObject()) {
          clients++;
        }
        it->release();
      }
      sm6125_hid_diag[6] = flags | (clients << 4);
    }
    OSNumber *page = OSDynamicCast(OSNumber, getProperty("PrimaryUsagePage"));
    OSNumber *usage = OSDynamicCast(OSNumber, getProperty("PrimaryUsage"));
    sm6125_hid_diag[7] = ((page ? page->unsigned32BitValue() : 0u) << 16) |
                         (usage ? (usage->unsigned32BitValue() & 0xFFFFu) : 0u);
    return eventDriverFound;
  }

  static IORegistryEntry *eventDriver(IORegistryEntry *parent, unsigned depth)
  {
    if (!depth || !parent) {
      return nullptr;
    }
    OSIterator *it = parent->getChildIterator(gIOServicePlane);
    if (!it) {
      return nullptr;
    }
    IORegistryEntry *found = nullptr;
    while (OSObject *child = it->getNextObject()) {
      IORegistryEntry *entry = OSDynamicCast(IORegistryEntry, child);
      if (!entry) continue;
      found = entry->metaCast("IOHIDEventDriver") ? entry : eventDriver(entry, depth - 1);
      if (found) break;
    }
    it->release();
    return found;
  }

  static void tick(OSObject *owner, IOTimerEventSource *source)
  {
    Sm6125HidInput *self = OSDynamicCast(Sm6125HidInput, owner);
    if (!self) return;
    (void)self->hasEventDriverCached();
    if (source) source->setTimeoutMS(1000);
  }

public:
  IOWorkLoop *getWorkLoop() const override { return inputLoop; }
  OSString *newTransportString() const override { return OSString::withCString("SPI"); }
  OSString *newProductString() const override { return OSString::withCString("Novatek NT36672A Touch Screen"); }

  IOReturn newReportDescriptor(IOMemoryDescriptor **out) const override
  {
    if (!out) {
      return kIOReturnBadArgument;
    }
    unsigned char combined[sizeof(kTouchDescriptor) + sizeof(kKeyboardConsumerDescriptor)];
    memcpy(combined, kTouchDescriptor, sizeof(kTouchDescriptor));
    memcpy(combined + sizeof(kTouchDescriptor), kKeyboardConsumerDescriptor, sizeof(kKeyboardConsumerDescriptor));
    *out = IOBufferMemoryDescriptor::withBytes(combined, sizeof(combined), kIODirectionNone);
    return *out ? kIOReturnSuccess : kIOReturnNoMemory;
  }

  bool start(IOService *provider) override
  {
    inputLoop = IOWorkLoop::workLoop();
    if (!inputLoop) return false;
    if (!setProperty("HIDDefaultBehavior", true)) return false;
    setProperty("Built-In", true);
    setProperty("DisplayIntegrated", true);
    setProperty("DigitizerCollectionDispatch", true);
    if (!IOHIDDevice::start(provider)) return false;
    timer = IOTimerEventSource::timerEventSource(this, tick);
    if (!timer || inputLoop->addEventSource(timer) != kIOReturnSuccess) {
      OSSafeReleaseNULL(timer);
      IOHIDDevice::stop(provider);
      return false;
    }
    sm6125_touch_set_sink(&Sm6125HidInput::touchNotify, this);
    sm6125_buttons_set_sink(&Sm6125HidInput::buttonsNotify, this);
    sm6125_hid_diag[0] = 1u;
    timer->setTimeoutMS(500);
    return true;
  }

  void stop(IOService *provider) override
  {
    sm6125_touch_set_sink(nullptr, nullptr);
    sm6125_buttons_set_sink(nullptr, nullptr);
    if (timer) {
      timer->cancelTimeout();
      inputLoop->removeEventSource(timer);
      OSSafeReleaseNULL(timer);
    }
    IOHIDDevice::stop(provider);
  }

  void free() override
  {
    OSSafeReleaseNULL(inputLoop);
    IOHIDDevice::free();
  }
};

OSDefineMetaClassAndStructors(Sm6125HidInput, IOHIDDevice)

extern "C" IOService *sm6125_hid_input_allocate(void)
{
  return new Sm6125HidInput;
}
