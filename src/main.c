
 /* WiiredX - v0.6.5 (rumble works in Pro Controller mode too)**
   Aroma plugin: wired Xbox One controllers on Wii U. Plug and play.
 
   A background thread owns the USB side (find pad, wake it, read packets).
   A hook on VPADRead merges the pad's state into what the game sees. */
 
#include <string.h>

#include <wups.h>
#include <wups/config/WUPSConfigItemBoolean.h>
#include <wups/config/WUPSConfigItemIntegerRange.h>
#include <wups/config/WUPSConfigItemMultipleValues.h>

#include <coreinit/energysaver.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/systeminfo.h>
#include <coreinit/title.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <nsysuhs/uhs.h>
#include <sysapp/switch.h>
#include <padscore/kpad.h>
#include <padscore/wpad.h>
#include <vpad/input.h>
#include <whb/log.h>
#include <whb/log_udp.h>

WUPS_PLUGIN_NAME("WiiredX");
WUPS_PLUGIN_DESCRIPTION("Wired Xbox One controllers on Wii U. Plug in and play.");
WUPS_PLUGIN_VERSION("v0.6.5");
WUPS_PLUGIN_AUTHOR("snoots");
WUPS_PLUGIN_LICENSE("MIT");

WUPS_USE_STORAGE("wiiredx");          /* settings are saved on the SD card under this name */

/*settings*/

#define LAYOUT_POSITION 0   /* Xbox A (bottom button) acts as Nintendo B (bottom button) */
#define LAYOUT_LABEL    1   /* Xbox A acts as Nintendo A */

#define ACT_GAMEPAD 0   
#define ACT_PRO     1   /* pretend to be a Pro Controller on its own player slot */

static bool    sCfgEnabled     = true;
static int32_t sCfgActAs       = ACT_GAMEPAD;
static int32_t sCfgProSlot     = 1;    
static int32_t sCfgLayout      = LAYOUT_POSITION;
static int32_t sCfgDeadzone    = 15;   
static int32_t sCfgTriggerPct  = 30;   
static bool    sCfgRumble      = true;
static int32_t sCfgRumblePct   = 55;   
static bool    sCfgGuideHome   = true; 
static bool    sCfgKeepAwake   = true; 

#define STICK_EMU_LEVEL   0.5f

#define MAX_PROFILES    16
#define PKT_SIZE        64
#define STACK_SIZE      0x8000
#define CONFIG_BUF_SIZE 0x1380

#define GIP_IF_CLASS    0xFF
#define GIP_IF_SUBCLASS 0x47
#define GIP_IF_PROTOCOL 0xD0

/*shared pad state*/

typedef struct {
   volatile bool     connected;
   volatile uint8_t  btn0, btn1;      
   volatile uint16_t lt, rt;
   volatile int16_t  lx, ly, rx, ry;
   volatile bool     guide;
} PadState;

static PadState sPad;


#define AWAKE_WINDOW_MS 60000

static volatile OSTime sLastInput;
static bool            sDimSuppressed;
static bool            sDimWasOn;      
static bool            sApdWasOn;


static uint32_t      sPrevHold;    
static bool          sPrevGuide;
static volatile bool sInOverlay;   


typedef struct {
   UhsHandle            handle;
   UhsConfig            config;
   UhsInterfaceProfile *profiles;
   uint8_t             *inBuf;
   uint8_t             *outBuf;
   OSThread            *thread;
   uint8_t             *stack;
   uint8_t             *rumbleBuf;
   OSThread            *rumbleThread;
   uint8_t             *rumbleStack;
   uint8_t              rumbleSeq;

   uint32_t ifHandle;
   uint16_t pid;
   uint8_t  inEp, outEp;
   uint32_t epMask;
   uint8_t  outSeq;
   bool     acquired;
} UsbCtx;

static UsbCtx          *sCtx;
static volatile bool    sRun;
static volatile bool    sThreadDone;
static volatile bool    sRumbleDone;

static uint8_t           sRumblePattern[15];
static volatile uint8_t  sRumbleLen;          
static volatile OSTime   sRumbleStart;
static bool             sLogInit;

static bool gamePadMode(void);          
static void resetAnnounce(void);
static void announceIfNeeded(int32_t chan);
static void enableProSupport(void);     


static uint32_t
msNow(void)
{
   return (uint32_t)(OSTicksToMilliseconds(OSGetTime()) % 1000000);
}

static void *
appAlloc(uint32_t size, int32_t align)
{
   void *p = MEMAllocFromDefaultHeapEx(size, align);
   if (p) {
      memset(p, 0, size);
   }
   return p;
}

static void
acquireCallback(void *context, int32_t arg1, int32_t arg2)
{
}

static int32_t
sendPacket(UsbCtx *c, const uint8_t *data, int32_t len, int seqOverride)
{
   memset(c->outBuf, 0, PKT_SIZE);
   memcpy(c->outBuf, data, len);
   if (seqOverride >= 0) {
      c->outBuf[2] = (uint8_t)seqOverride;
   } else {
      c->outBuf[2] = c->outSeq++;
      if (c->outSeq == 0) {
         c->outSeq = 1;
      }
   }
   return (int32_t)UhsSubmitInterruptRequest(&c->handle, c->ifHandle, c->outEp,
                                             ENDPOINT_TRANSFER_OUT,
                                             c->outBuf, len, TIMEOUT_NONE);
}

static bool
findController(UsbCtx *c)
{
   UhsInterfaceFilter filter;
   memset(&filter, 0, sizeof(filter));
   filter.match_params = MATCH_ANY;

   memset(c->profiles, 0, sizeof(UhsInterfaceProfile) * MAX_PROFILES);
   int32_t count = (int32_t)UhsQueryInterfaces(&c->handle, &filter, c->profiles, MAX_PROFILES);

   for (int i = 0; i < count && i < MAX_PROFILES; i++) {
      UhsInterfaceProfile *p = &c->profiles[i];
      if (p->if_desc.bInterfaceClass    != GIP_IF_CLASS    ||
          p->if_desc.bInterfaceSubClass != GIP_IF_SUBCLASS ||
          p->if_desc.bInterfaceProtocol != GIP_IF_PROTOCOL ||
          p->if_desc.bNumEndpoints < 2) {
         continue;
      }

      c->ifHandle = p->if_handle;
      c->pid      = p->dev_desc.idProduct;
      c->inEp = c->outEp = 0;
      c->epMask   = 0;

      for (int e = 0; e < 16; e++) {
         UhsEndpointDescriptor *in  = &p->in_endpoints[e];
         UhsEndpointDescriptor *out = &p->out_endpoints[e];
         if (in->bLength && !c->inEp) {
            c->inEp = in->bEndpointAddress & 0x0F;
            c->epMask |= UHSEndpointGetMask(in);
         }
         if (out->bLength && !c->outEp) {
            c->outEp = out->bEndpointAddress & 0x0F;
            c->epMask |= UHSEndpointGetMask(out);
         }
      }
      return c->inEp && c->outEp;
   }
   return false;
}

static bool
startController(UsbCtx *c)
{
   static const uint8_t powerOn[] = { 0x05, 0x20, 0x00, 0x01, 0x00 };
   static const uint8_t sInit[]   = { 0x05, 0x20, 0x00, 0x0F, 0x06 };

   int32_t r = (int32_t)UhsAcquireInterface(&c->handle, c->ifHandle, NULL, acquireCallback);
   WHBLogPrintf("wiiredx %u: acquire -> %08X", msNow(), (unsigned)r);
   if (r < 0) {
      return false;
   }
   c->acquired = true;

   r = (int32_t)UhsAdministerEndpoint(&c->handle, c->ifHandle, UHS_ADMIN_EP_ENABLE,
                                      c->epMask, 4, PKT_SIZE);
   WHBLogPrintf("wiiredx %u: ep enable -> %08X", msNow(), (unsigned)r);
   if (r < 0) {
      return false;
   }

   c->outSeq = 1;
   r = sendPacket(c, powerOn, sizeof(powerOn), -1);
   WHBLogPrintf("wiiredx %u: power on -> %08X", msNow(), (unsigned)r);
   if (r < 0) {
      return false;
   }
   if (c->pid == 0x02EA || c->pid == 0x0B00) {
      sendPacket(c, sInit, sizeof(sInit), -1);
   }
   return true;
}

static void
stopController(UsbCtx *c)
{
   memset(&sPad, 0, sizeof(sPad));
   if (c->acquired) {
      WHBLogPrintf("wiiredx %u: releasing interface", msNow());
      UhsAdministerEndpoint(&c->handle, c->ifHandle, UHS_ADMIN_EP_CANCEL, c->epMask, 0, 0);
      UhsReleaseInterface(&c->handle, c->ifHandle, false);
      c->acquired = false;
   }
}


static int
stillPresent(UsbCtx *c)
{
   UhsInterfaceFilter filter;
   memset(&filter, 0, sizeof(filter));
   filter.match_params = MATCH_ANY;

   memset(c->profiles, 0, sizeof(UhsInterfaceProfile) * MAX_PROFILES);
   int32_t count = (int32_t)UhsQueryInterfaces(&c->handle, &filter, c->profiles, MAX_PROFILES);
   if (count < 0) {
      return -1;
   }
   for (int i = 0; i < count && i < MAX_PROFILES; i++) {
      if (c->profiles[i].if_handle == c->ifHandle) {
         return 1;
      }
   }
   return 0;
}

static int
usbThread(int argc, const char **argv)
{
   UsbCtx *c = sCtx;

   while (sRun) {
      
      if (!findController(c)) {
         WHBLogPrintf("wiiredx %u: no pad found", msNow());
         for (int i = 0; i < 3 && sRun; i++) {
            OSSleepTicks(OSMillisecondsToTicks(100));
         }
         continue;
      }
      WHBLogPrintf("wiiredx %u: found pad, claiming it", msNow());
      if (!startController(c)) {
         stopController(c);
         for (int i = 0; i < 3 && sRun; i++) {
            OSSleepTicks(OSMillisecondsToTicks(100));
         }
         continue;
      }

      WHBLogPrintf("wiiredx %u: controller ready", msNow());
      resetAnnounce();
      sPad.connected = true;

      
      const char *reason = "stopping";
      uint32_t lastCode  = 0;

      while (sRun) {
         int32_t ret = (int32_t)UhsSubmitInterruptRequest(&c->handle, c->ifHandle, c->inEp,
                                                          ENDPOINT_TRANSFER_IN,
                                                          c->inBuf, PKT_SIZE, TIMEOUT_NONE);
         if (ret < 0) {
            if ((uint32_t)ret != lastCode) {          
               lastCode = (uint32_t)ret;
               WHBLogPrintf("wiiredx %u: read error %08X (%s)", msNow(), (unsigned)ret,
                            sInOverlay ? "in background" : "in foreground");
            }
            if (stillPresent(c) == 0) {
               reason = "pad re-enumerated or unplugged";
               break;
            }
            OSSleepTicks(OSMillisecondsToTicks(50));
            continue;
         }
         lastCode = 0;

         const uint8_t *d = c->inBuf;

         if (d[0] == 0x20 && ret >= 18) {                 
            sLastInput = OSGetTime();                     
            sPad.btn0 = d[4];
            sPad.btn1 = d[5];
            sPad.lt   = d[6]  | (d[7]  << 8);
            sPad.rt   = d[8]  | (d[9]  << 8);
            sPad.lx   = (int16_t)(d[10] | (d[11] << 8));
            sPad.ly   = (int16_t)(d[12] | (d[13] << 8));
            sPad.rx   = (int16_t)(d[14] | (d[15] << 8));
            sPad.ry   = (int16_t)(d[16] | (d[17] << 8));
         } else if (d[0] == 0x07 && ret >= 5) {           
            sPad.guide = (d[4] & 0x01) != 0;
            if (d[1] & 0x10) {                            
               static const uint8_t ack[13] = { 0x01, 0x20, 0x00, 0x09, 0x00, 0x07, 0x20,
                                                0x02, 0x00, 0x00, 0x00, 0x00, 0x00 };
               sendPacket(c, ack, sizeof(ack), d[2]);
            }
         }
      }

      WHBLogPrintf("wiiredx %u: controller gone (%s)", msNow(), reason);
      stopController(c);
   }

   sThreadDone = true;
   return 0;
}

/*rumble bullshit*/
static volatile bool sProMotorOn;

static int
rumbleLevel(void)
{
   if (!gamePadMode()) {
      return sProMotorOn ? 4 : 0;
   }

   uint8_t len = sRumbleLen;
   if (!len) {
      return 0;
   }
   int64_t ms = (int64_t)OSTicksToMilliseconds(OSGetTime() - sRumbleStart);
   if (ms < 0) {
      return 0;
   }
   uint32_t bit = (uint32_t)(ms * 120 / 1000);
   if (bit >= len) {
      return 0;                       
   }
   int ones = 0;
   for (uint32_t i = bit; i < bit + 4 && i < len; i++) {
      if (sRumblePattern[i / 8] & (0x80 >> (i % 8))) {
         ones++;
      }
   }
   return ones;
}

static void
sendRumble(UsbCtx *c, int level)
{
   if (!c->acquired) {
      return;
   }
   uint8_t *b = c->rumbleBuf;
   memset(b, 0, PKT_SIZE);
   b[0]  = 0x09;                              
   b[2]  = c->rumbleSeq++;
   b[3]  = 0x09;                              
   b[5]  = 0x0F;                              
   b[8]  = (uint8_t)(sCfgRumblePct * level / 4);            
   b[9]  = (uint8_t)(sCfgRumblePct * 65 / 100 * level / 4); 
   b[10] = 0xFF;                              
   b[11] = 0x00;                              
   b[12] = 0xFF;                              
   UhsSubmitInterruptRequest(&c->handle, c->ifHandle, c->outEp,
                             ENDPOINT_TRANSFER_OUT, b, 13, TIMEOUT_NONE);
}


static void
updateScreenAwake(void)
{
   bool wantAwake = false;

   if (sCfgKeepAwake && sCfgEnabled && sPad.connected && sLastInput) {
      wantAwake = OSTicksToMilliseconds(OSGetTime() - sLastInput) < AWAKE_WINDOW_MS;
   }
   if (wantAwake == sDimSuppressed) {
      return;
   }

   if (wantAwake) {
      if (sDimWasOn) {
         IMDisableDim();
      }
      if (sApdWasOn) {
         IMDisableAPD();
      }
   } else {
      if (sDimWasOn) {
         IMEnableDim();
      }
      if (sApdWasOn) {
         IMEnableAPD();
      }
   }
   sDimSuppressed = wantAwake;
   WHBLogPrintf("wiiredx %u: screen dimming %s", msNow(), wantAwake ? "off" : "back on");
}

static int
rumbleThread(int argc, const char **argv)
{
   UsbCtx *c      = sCtx;
   int lastLevel  = 0;
   OSTime lastSend = 0;
   int    awakeTick = 0;

   while (sRun) {
      OSSleepTicks(OSMillisecondsToTicks(20));
      if (++awakeTick >= 50) {          /* about once a second i think*/
         awakeTick = 0;
         updateScreenAwake();
      }
      if (!sPad.connected) {
         lastLevel = 0;
         continue;
      }
      int level  = (sCfgRumble && sCfgEnabled) ? rumbleLevel() : 0;
      OSTime now = OSGetTime();
      
      bool refresh = level && OSTicksToMilliseconds(now - lastSend) > 1000;
      if (level != lastLevel || refresh) {
         sendRumble(c, level);
         lastLevel = level;
         lastSend  = now;
      }
   }

   if (lastLevel && sPad.connected) {
      sendRumble(c, 0);               
   }
   sRumbleDone = true;
   return 0;
}

static float
stickAxis(int16_t raw)
{
   float v = raw / 32767.0f;
   if (v < -1.0f) {
      v = -1.0f;
   }
   float a  = v < 0 ? -v : v;
   float dz = sCfgDeadzone / 100.0f;
   if (a < dz) {
      return 0.0f;
   }
   
   a = (a - dz) / (1.0f - dz);
   return v < 0 ? -a : a;
}

/*pro controller mode*/

static uint32_t            sProPrevHold;
static bool                sProSupportOn;   
static WPADConnectCallback sGameConnectCb[4];
static int                 sAnnounceCount[4];   
static OSTime              sNextAnnounce[4];    


static bool
isWiiUMenu(void)
{
   return (OSGetTitleID() & ~0xFFull) == 0x0005001010040000ull;
}

static bool
gamePadMode(void)
{
   return sCfgActAs == ACT_GAMEPAD || isWiiUMenu();
}

static bool
proSlotIs(int32_t chan)
{
   return sCfgEnabled && !gamePadMode() && sPad.connected &&
          chan == sCfgProSlot - 1;
}

static uint32_t
buildProButtons(float lx, float ly, float rx, float ry)
{
   uint32_t h = 0;
   uint8_t b0 = sPad.btn0, b1 = sPad.btn1;

   if (sCfgLayout == LAYOUT_POSITION) {
      if (b0 & 0x10) h |= WPAD_PRO_BUTTON_B;   
      if (b0 & 0x20) h |= WPAD_PRO_BUTTON_A;   
      if (b0 & 0x40) h |= WPAD_PRO_BUTTON_Y;   
      if (b0 & 0x80) h |= WPAD_PRO_BUTTON_X;   
   } else {
      if (b0 & 0x10) h |= WPAD_PRO_BUTTON_A;
      if (b0 & 0x20) h |= WPAD_PRO_BUTTON_B;
      if (b0 & 0x40) h |= WPAD_PRO_BUTTON_X;
      if (b0 & 0x80) h |= WPAD_PRO_BUTTON_Y;
   }
   if (b0 & 0x04) h |= WPAD_PRO_BUTTON_PLUS;   
   if (b0 & 0x08) h |= WPAD_PRO_BUTTON_MINUS;  

   if (b1 & 0x01) h |= WPAD_PRO_BUTTON_UP;
   if (b1 & 0x02) h |= WPAD_PRO_BUTTON_DOWN;
   if (b1 & 0x04) h |= WPAD_PRO_BUTTON_LEFT;
   if (b1 & 0x08) h |= WPAD_PRO_BUTTON_RIGHT;
   if (b1 & 0x10) h |= WPAD_PRO_BUTTON_L;
   if (b1 & 0x20) h |= WPAD_PRO_BUTTON_R;
   if (b1 & 0x40) h |= WPAD_PRO_BUTTON_STICK_L;
   if (b1 & 0x80) h |= WPAD_PRO_BUTTON_STICK_R;

   uint16_t trigLevel = (uint16_t)(1023 * sCfgTriggerPct / 100);
   if (sPad.lt > trigLevel) h |= WPAD_PRO_BUTTON_ZL;
   if (sPad.rt > trigLevel) h |= WPAD_PRO_BUTTON_ZR;

   if (lx < -STICK_EMU_LEVEL) h |= WPAD_PRO_STICK_L_EMULATION_LEFT;
   if (lx >  STICK_EMU_LEVEL) h |= WPAD_PRO_STICK_L_EMULATION_RIGHT;
   if (ly >  STICK_EMU_LEVEL) h |= WPAD_PRO_STICK_L_EMULATION_UP;
   if (ly < -STICK_EMU_LEVEL) h |= WPAD_PRO_STICK_L_EMULATION_DOWN;
   if (rx < -STICK_EMU_LEVEL) h |= WPAD_PRO_STICK_R_EMULATION_LEFT;
   if (rx >  STICK_EMU_LEVEL) h |= WPAD_PRO_STICK_R_EMULATION_RIGHT;
   if (ry >  STICK_EMU_LEVEL) h |= WPAD_PRO_STICK_R_EMULATION_UP;
   if (ry < -STICK_EMU_LEVEL) h |= WPAD_PRO_STICK_R_EMULATION_DOWN;
   return h;
}

static void
fillProStatus(KPADStatus *k)
{
   memset(k, 0, sizeof(*k));

   float lx = stickAxis(sPad.lx), ly = stickAxis(sPad.ly);
   float rx = stickAxis(sPad.rx), ry = stickAxis(sPad.ry);
   uint32_t hold = buildProButtons(lx, ly, rx, ry);

   k->extensionType = WPAD_EXT_PRO_CONTROLLER;
   k->format        = WPAD_FMT_PRO_CONTROLLER;
   k->error         = KPAD_ERROR_OK;
   k->posValid      = 0;

   k->pro.hold      = hold;
   k->pro.trigger   = hold & ~sProPrevHold;
   k->pro.release   = sProPrevHold & ~hold;
   sProPrevHold     = hold;

   k->pro.leftStick.x  = lx;
   k->pro.leftStick.y  = ly;
   k->pro.rightStick.x = rx;
   k->pro.rightStick.y = ry;
   k->pro.wired        = 1;
   k->pro.charging     = 0;
}


static void
enableProSupport(void)
{
   if (sProSupportOn) {
      return;
   }
   sProSupportOn = true;
   KPADInit();
   WPADEnableURCC(TRUE);
   WHBLogPrintf("wiiredx %u: turned on Pro Controller support", msNow());
}



#define ANNOUNCE_TRIES 4

static void
resetAnnounce(void)
{
   for (int i = 0; i < 4; i++) {
      sAnnounceCount[i] = 0;
      sNextAnnounce[i]  = 0;
   }
}

static void
announceIfNeeded(int32_t chan)
{
   if (chan < 0 || chan > 3 || !sGameConnectCb[chan] ||
       sAnnounceCount[chan] >= ANNOUNCE_TRIES) {
      return;
   }
   OSTime now = OSGetTime();
   if (sAnnounceCount[chan] > 0 && now < sNextAnnounce[chan]) {
      return;
   }

   static const uint32_t gapMs[ANNOUNCE_TRIES] = { 1000, 2000, 4000, 0 };
   int n = sAnnounceCount[chan]++;
   sNextAnnounce[chan] = now + (OSTime)OSMillisecondsToTicks(gapMs[n]);

   WHBLogPrintf("wiiredx %u: telling the game a Pro Controller connected on slot %d (try %d)",
                msNow(), (int)chan + 1, n + 1);
   sGameConnectCb[chan]((WPADChan)chan, WPAD_ERROR_NONE);
}

DECL_FUNCTION(uint32_t, KPADReadEx, KPADChan chan, KPADStatus *data, uint32_t count, KPADError *error)
{
   uint32_t result = real_KPADReadEx(chan, data, count, error);

   if (proSlotIs((int32_t)chan) && data && count > 0) {
      announceIfNeeded((int32_t)chan);
      fillProStatus(&data[0]);
      if (error) {
         *error = KPAD_ERROR_OK;
      }
      if (result == 0) {
         result = 1;
      }
   }
   return result;
}

DECL_FUNCTION(uint32_t, KPADRead, KPADChan chan, KPADStatus *data, uint32_t count)
{
   uint32_t result = real_KPADRead(chan, data, count);

   if (proSlotIs((int32_t)chan) && data && count > 0) {
      announceIfNeeded((int32_t)chan);
      fillProStatus(&data[0]);
      if (result == 0) {
         result = 1;
      }
   }
   return result;
}

DECL_FUNCTION(WPADError, WPADProbe, WPADChan channel, WPADExtensionType *outExtensionType)
{
   WPADError result = real_WPADProbe(channel, outExtensionType);

   if (proSlotIs((int32_t)channel)) {
      announceIfNeeded((int32_t)channel);
      if (outExtensionType) {
         *outExtensionType = WPAD_EXT_PRO_CONTROLLER;
      }
      result = WPAD_ERROR_NONE;
   }
   return result;
}


DECL_FUNCTION(KPADConnectCallback, KPADSetConnectCallback, KPADChan chan, KPADConnectCallback callback)
{
   if ((int32_t)chan >= 0 && (int32_t)chan <= 3) {
      sGameConnectCb[chan] = callback;
      sAnnounceCount[chan] = 0;          
      sNextAnnounce[chan]  = 0;
   }
   return real_KPADSetConnectCallback(chan, callback);
}

DECL_FUNCTION(WPADConnectCallback, WPADSetConnectCallback, WPADChan channel, WPADConnectCallback callback)
{
   if ((int32_t)channel >= 0 && (int32_t)channel <= 3) {
      sGameConnectCb[channel] = callback;
      sAnnounceCount[channel] = 0;
      sNextAnnounce[channel]  = 0;
   }
   return real_WPADSetConnectCallback(channel, callback);
}

DECL_FUNCTION(void, WPADControlMotor, WPADChan channel, BOOL motorEnabled)
{
   if (proSlotIs((int32_t)channel)) {
      sProMotorOn = motorEnabled ? true : false;
   }
   real_WPADControlMotor(channel, motorEnabled);
}

WUPS_MUST_REPLACE(WPADControlMotor, WUPS_LOADER_LIBRARY_PADSCORE, WPADControlMotor);
WUPS_MUST_REPLACE(KPADReadEx, WUPS_LOADER_LIBRARY_PADSCORE, KPADReadEx);
WUPS_MUST_REPLACE(KPADRead, WUPS_LOADER_LIBRARY_PADSCORE, KPADRead);
WUPS_MUST_REPLACE(WPADProbe, WUPS_LOADER_LIBRARY_PADSCORE, WPADProbe);
WUPS_MUST_REPLACE(KPADSetConnectCallback, WUPS_LOADER_LIBRARY_PADSCORE, KPADSetConnectCallback);
WUPS_MUST_REPLACE(WPADSetConnectCallback, WUPS_LOADER_LIBRARY_PADSCORE, WPADSetConnectCallback);



#define KEY_ENABLED  "enabled"
#define KEY_LAYOUT   "layout"
#define KEY_DEADZONE "deadzone"
#define KEY_TRIGGER  "trigger"
#define KEY_RUMBLE   "rumble"
#define KEY_RUMBLEPC "rumblePct"
#define KEY_GUIDE    "guideHome"
#define KEY_ACTAS    "actAs"
#define KEY_PROSLOT  "proSlot"
#define KEY_AWAKE    "keepAwake"

static void onEnabled(ConfigItemBoolean *i, bool v)        { sCfgEnabled    = v; WUPSStorageAPI_StoreBool(NULL, KEY_ENABLED, v); }
static void onRumble(ConfigItemBoolean *i, bool v)         { sCfgRumble     = v; WUPSStorageAPI_StoreBool(NULL, KEY_RUMBLE, v); }
static void onGuide(ConfigItemBoolean *i, bool v)          { sCfgGuideHome  = v; WUPSStorageAPI_StoreBool(NULL, KEY_GUIDE, v); }
static void onAwake(ConfigItemBoolean *i, bool v)          { sCfgKeepAwake  = v; WUPSStorageAPI_StoreBool(NULL, KEY_AWAKE, v); }
static void onLayout(ConfigItemMultipleValues *i, uint32_t v) { sCfgLayout  = (int32_t)v; WUPSStorageAPI_StoreInt(NULL, KEY_LAYOUT, (int32_t)v); }
static void onActAs(ConfigItemMultipleValues *i, uint32_t v)  { sCfgActAs   = (int32_t)v; WUPSStorageAPI_StoreInt(NULL, KEY_ACTAS, (int32_t)v); }
static void onProSlot(ConfigItemIntegerRange *i, int32_t v)   { sCfgProSlot = v; WUPSStorageAPI_StoreInt(NULL, KEY_PROSLOT, v); }
static void onDeadzone(ConfigItemIntegerRange *i, int32_t v)  { sCfgDeadzone   = v; WUPSStorageAPI_StoreInt(NULL, KEY_DEADZONE, v); }
static void onTrigger(ConfigItemIntegerRange *i, int32_t v)   { sCfgTriggerPct = v; WUPSStorageAPI_StoreInt(NULL, KEY_TRIGGER, v); }
static void onRumblePct(ConfigItemIntegerRange *i, int32_t v) { sCfgRumblePct  = v; WUPSStorageAPI_StoreInt(NULL, KEY_RUMBLEPC, v); }

static WUPSConfigAPICallbackStatus
menuOpened(WUPSConfigCategoryHandle root)
{
   static ConfigItemMultipleValuesPair layouts[] = {
      { LAYOUT_POSITION, "By position (A = bottom)" },
      { LAYOUT_LABEL,    "By label (A = A)" },
   };

   WUPSConfigItemBoolean_AddToCategory(root, KEY_ENABLED, "Use Xbox controller", true, sCfgEnabled, onEnabled);
   static ConfigItemMultipleValuesPair actAs[] = {
      { ACT_GAMEPAD, "GamePad" },
      { ACT_PRO,     "Pro Controller" },
   };
   WUPSConfigItemMultipleValues_AddToCategory(root, KEY_ACTAS, "Act as", ACT_GAMEPAD, sCfgActAs,
                                              actAs, 2, onActAs);
   WUPSConfigItemIntegerRange_AddToCategory(root, KEY_PROSLOT, "Pro Controller player slot", 1, sCfgProSlot, 1, 4, onProSlot);
   WUPSConfigItemMultipleValues_AddToCategory(root, KEY_LAYOUT, "Face buttons", LAYOUT_POSITION, sCfgLayout,
                                              layouts, 2, onLayout);
   WUPSConfigItemIntegerRange_AddToCategory(root, KEY_DEADZONE, "Stick deadzone (%)", 15, sCfgDeadzone, 0, 40, onDeadzone);
   WUPSConfigItemIntegerRange_AddToCategory(root, KEY_TRIGGER, "Trigger press point (%)", 30, sCfgTriggerPct, 5, 95, onTrigger);
   WUPSConfigItemBoolean_AddToCategory(root, KEY_GUIDE, "Guide button opens HOME Menu", true, sCfgGuideHome, onGuide);
   WUPSConfigItemBoolean_AddToCategory(root, KEY_AWAKE, "Keep screen awake", true, sCfgKeepAwake, onAwake);
   WUPSConfigItemBoolean_AddToCategory(root, KEY_RUMBLE, "Rumble", true, sCfgRumble, onRumble);
   WUPSConfigItemIntegerRange_AddToCategory(root, KEY_RUMBLEPC, "Rumble strength (%)", 55, sCfgRumblePct, 0, 100, onRumblePct);
   return WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS;
}

static void
menuClosed(void)
{
   WUPSStorageAPI_SaveStorage(false);      /* write changes to the SD card */
}


static void
loadBool(const char *key, bool *value)
{
   if (WUPSStorageAPI_GetBool(NULL, key, value) == WUPS_STORAGE_ERROR_NOT_FOUND) {
      WUPSStorageAPI_StoreBool(NULL, key, *value);
   }
}

static void
loadInt(const char *key, int32_t *value, int32_t min, int32_t max)
{
   int32_t v = *value;
   if (WUPSStorageAPI_GetInt(NULL, key, &v) == WUPS_STORAGE_ERROR_NOT_FOUND) {
      WUPSStorageAPI_StoreInt(NULL, key, *value);
   } else if (v >= min && v <= max) {
      *value = v;
   }
}


INITIALIZE_PLUGIN()
{
   WUPSConfigAPIOptionsV1 options = { .name = "WiiredX" };
   WUPSConfigAPI_Init(options, menuOpened, menuClosed);

   loadBool(KEY_ENABLED, &sCfgEnabled);
   loadInt(KEY_ACTAS, &sCfgActAs, 0, 1);
   loadInt(KEY_PROSLOT, &sCfgProSlot, 1, 4);
   loadInt(KEY_LAYOUT, &sCfgLayout, 0, 1);
   loadInt(KEY_DEADZONE, &sCfgDeadzone, 0, 40);
   loadInt(KEY_TRIGGER, &sCfgTriggerPct, 5, 95);
   loadBool(KEY_RUMBLE, &sCfgRumble);
   loadBool(KEY_GUIDE, &sCfgGuideHome);
   loadBool(KEY_AWAKE, &sCfgKeepAwake);
   loadInt(KEY_RUMBLEPC, &sCfgRumblePct, 0, 100);
   WUPSStorageAPI_SaveStorage(false);
}



ON_APPLICATION_START()
{
   sLogInit = WHBLogUdpInit();
   memset(&sPad, 0, sizeof(sPad));
   uint32_t on = 0;
   sDimWasOn      = (IMIsDimEnabled(&on) == 0) && on;
   on             = 0;
   sApdWasOn      = (IMIsAPDEnabled(&on) == 0) && on;
   sDimSuppressed = false;
   sLastInput     = 0;

   sInOverlay    = false;
   sPrevGuide    = false;
   sPrevHold     = 0;
   sProSupportOn = false;
   sProMotorOn   = false;

   UsbCtx *c = appAlloc(sizeof(UsbCtx), 0x40);
   if (!c) {
      return;
   }
   c->profiles      = appAlloc(sizeof(UhsInterfaceProfile) * MAX_PROFILES, 0x40);
   c->inBuf         = appAlloc(PKT_SIZE, 0x40);
   c->outBuf        = appAlloc(PKT_SIZE, 0x40);
   c->thread        = appAlloc(sizeof(OSThread), 0x10);
   c->stack         = appAlloc(STACK_SIZE, 0x10);
   c->config.buffer = appAlloc(CONFIG_BUF_SIZE, 0x40);
   c->rumbleBuf     = appAlloc(PKT_SIZE, 0x40);
   c->rumbleThread  = appAlloc(sizeof(OSThread), 0x10);
   c->rumbleStack   = appAlloc(STACK_SIZE, 0x10);
   if (!c->profiles || !c->inBuf || !c->outBuf || !c->thread || !c->stack || !c->config.buffer ||
       !c->rumbleBuf || !c->rumbleThread || !c->rumbleStack) {
      WHBLogPrintf("wiiredx: out of memory");
      return;                       /* the game's heap is freed when it exits */
   }
   c->config.controller_num = 0;    /* external USB ports */
   c->config.buffer_size    = UHS_CONFIG_BUFFER_SIZE;

   int32_t st = (int32_t)UhsClientOpen(&c->handle, &c->config);
   WHBLogPrintf("wiiredx: open -> %08X", (unsigned)st);
   if (st != 0) {
      return;
   }

   sCtx        = c;
   sRun        = true;
   sThreadDone = false;
   sRumbleDone = true;
   sRumbleLen  = 0;
   if (!OSCreateThread(c->thread, usbThread, 0, NULL,
                       c->stack + STACK_SIZE, STACK_SIZE,
                       15, OS_THREAD_ATTRIB_AFFINITY_ANY)) {
      sCtx = NULL;
      sRun = false;
      return;
   }
   OSSetThreadName(c->thread, "wiiredx usb");
   OSResumeThread(c->thread);

   sRumbleDone = false;
   if (OSCreateThread(c->rumbleThread, rumbleThread, 0, NULL,
                      c->rumbleStack + STACK_SIZE, STACK_SIZE,
                      15, OS_THREAD_ATTRIB_AFFINITY_ANY)) {
      OSSetThreadName(c->rumbleThread, "wiiredx rumble");
      OSResumeThread(c->rumbleThread);
   } else {
      sRumbleDone = true;
   }
}

ON_APPLICATION_ENDS()
{
   UsbCtx *c = sCtx;
   if (c) {
      sRun       = false;
      sRumbleLen = 0;
      
      for (int i = 0; i < 20 && !sRumbleDone; i++) {
         OSSleepTicks(OSMillisecondsToTicks(10));
      }
      stopController(c);            /* cancels the pending read */
      for (int i = 0; i < 40 && !sThreadDone; i++) {
         OSSleepTicks(OSMillisecondsToTicks(25));
      }
      if (sThreadDone) {
         UhsClientClose(&c->handle);
      }
      sCtx = NULL;
   }
   memset(&sPad, 0, sizeof(sPad));

   if (sDimSuppressed) {                
      if (sDimWasOn) {
         IMEnableDim();
      }
      if (sApdWasOn) {
         IMEnableAPD();
      }
      sDimSuppressed = false;
   }

   if (sLogInit) {
      WHBLogUdpDeinit();
      sLogInit = false;
   }
}

/*input injection (if this works rejoice)*/

static uint32_t
buildButtons(float lx, float ly, float rx, float ry)
{
   uint32_t h = 0;
   uint8_t b0 = sPad.btn0, b1 = sPad.btn1;

   if (sCfgLayout == LAYOUT_POSITION) {
      if (b0 & 0x10) h |= VPAD_BUTTON_B;   /* Xbox A (bottom) */
      if (b0 & 0x20) h |= VPAD_BUTTON_A;   /* Xbox B (right)  */
      if (b0 & 0x40) h |= VPAD_BUTTON_Y;   /* Xbox X (left)   */
      if (b0 & 0x80) h |= VPAD_BUTTON_X;   /* Xbox Y (top)    */
   } else {
      if (b0 & 0x10) h |= VPAD_BUTTON_A;
      if (b0 & 0x20) h |= VPAD_BUTTON_B;
      if (b0 & 0x40) h |= VPAD_BUTTON_X;
      if (b0 & 0x80) h |= VPAD_BUTTON_Y;
   }
   if (b0 & 0x04) h |= VPAD_BUTTON_PLUS;   /* Menu */
   if (b0 & 0x08) h |= VPAD_BUTTON_MINUS;  /* View */

   if (b1 & 0x01) h |= VPAD_BUTTON_UP;
   if (b1 & 0x02) h |= VPAD_BUTTON_DOWN;
   if (b1 & 0x04) h |= VPAD_BUTTON_LEFT;
   if (b1 & 0x08) h |= VPAD_BUTTON_RIGHT;
   if (b1 & 0x10) h |= VPAD_BUTTON_L;
   if (b1 & 0x20) h |= VPAD_BUTTON_R;
   if (b1 & 0x40) h |= VPAD_BUTTON_STICK_L;
   if (b1 & 0x80) h |= VPAD_BUTTON_STICK_R;

   uint16_t trigLevel = (uint16_t)(1023 * sCfgTriggerPct / 100);
   if (sPad.lt > trigLevel) h |= VPAD_BUTTON_ZL;
   if (sPad.rt > trigLevel) h |= VPAD_BUTTON_ZR;

   
   if (lx < -STICK_EMU_LEVEL) h |= VPAD_STICK_L_EMULATION_LEFT;
   if (lx >  STICK_EMU_LEVEL) h |= VPAD_STICK_L_EMULATION_RIGHT;
   if (ly >  STICK_EMU_LEVEL) h |= VPAD_STICK_L_EMULATION_UP;
   if (ly < -STICK_EMU_LEVEL) h |= VPAD_STICK_L_EMULATION_DOWN;
   if (rx < -STICK_EMU_LEVEL) h |= VPAD_STICK_R_EMULATION_LEFT;
   if (rx >  STICK_EMU_LEVEL) h |= VPAD_STICK_R_EMULATION_RIGHT;
   if (ry >  STICK_EMU_LEVEL) h |= VPAD_STICK_R_EMULATION_UP;
   if (ry < -STICK_EMU_LEVEL) h |= VPAD_STICK_R_EMULATION_DOWN;
   return h;
}


/*Guide -> HOME Menu (FUCK THIS THING)*/

static OSTime sLastHbm;

static void
checkGuideButton(void)
{
   bool guide = sPad.guide;
   bool pressed = guide && !sPrevGuide;
   sPrevGuide = guide;
   if (pressed) {
      WHBLogPrintf("wiiredx: Guide pressed (title %016llX)", (unsigned long long)OSGetTitleID());
   }

   if (!pressed || !sCfgGuideHome) {
      return;
   }
   OSTime now = OSGetTime();
   if (sLastHbm && OSTicksToMilliseconds(now - sLastHbm) < 1500) {
      return;
   }
   /* Not every game uses ProcUI (the log showed one that doesn't), so had to
      track foreground with the plugin system's hooks instead. */
   if (sInOverlay) {
      WHBLogPrintf("wiiredx: Guide ignored (app is in the background)");
      return;
   }
   if (!OSIsHomeButtonMenuEnabled()) {
      WHBLogPrintf("wiiredx: HOME Menu is disabled right now");
      return;
   }
   sLastHbm = now;
   int32_t r = _SYSSwitchToHBMWithMode(0);
   WHBLogPrintf("wiiredx: open HOME Menu -> %d", (int)r);
}

/* HOME Menu support (FUCK THIS^2)*/

static volatile uint32_t sOverlayLatch;   
static volatile bool     sOverlayGuideLatch;

ON_RELEASE_FOREGROUND()
{
   
   sInOverlay         = true;
   sOverlayLatch      = 0xFFFFFFFF;        /* resolved on the first menu read */
   sOverlayGuideLatch = true;
   sPrevHold          = 0;
   WHBLogPrintf("wiiredx %u: release foreground", msNow());
}

ON_ACQUIRED_FOREGROUND()
{
   /* back in the game */
   sInOverlay = false;
   sPrevHold  = 0;
   sPrevGuide    = sPad.guide;
   sProSupportOn = false;      /* re-arm: switch Pro support on again */
   sProMotorOn   = false;
   resetAnnounce();               /* don't treat a held Guide as a new press */
   WHBLogPrintf("wiiredx %u: acquired foreground", msNow());
}


static int32_t
injectPad(VPADStatus *buffers, int32_t result, VPADReadError *realError, bool inHomeMenu)
{
   
   if (result <= 0) {
      memset(&buffers[0], 0, sizeof(VPADStatus));
      result     = 1;
      *realError = VPAD_READ_SUCCESS;
   }

   float lx = stickAxis(sPad.lx), ly = stickAxis(sPad.ly);
   float rx = stickAxis(sPad.rx), ry = stickAxis(sPad.ry);

   uint32_t hold = buildButtons(lx, ly, rx, ry);

   if (inHomeMenu) {
      bool guide = sPad.guide;
      if (sOverlayLatch == 0xFFFFFFFF) {
         sOverlayLatch = hold;             
      }
      sOverlayLatch &= hold;               
      hold &= ~sOverlayLatch;

      if (!guide) {
         sOverlayGuideLatch = false;
      }
      if (guide && !sOverlayGuideLatch) {
         hold |= VPAD_BUTTON_B;            
      }

      
      if (sOverlayLatch & (VPAD_STICK_L_EMULATION_LEFT | VPAD_STICK_L_EMULATION_RIGHT |
                           VPAD_STICK_L_EMULATION_UP | VPAD_STICK_L_EMULATION_DOWN)) {
         lx = ly = 0.0f;
      }
   } else {
      checkGuideButton();
   }

   VPADStatus *s = &buffers[0];                
   s->hold    |= hold;
   s->trigger |= hold & ~sPrevHold;            
   s->release |= sPrevHold & ~hold;            
   sPrevHold   = hold;

   /* Whichever device is actually moving a stick wins */
   if (lx != 0.0f || ly != 0.0f) {
      s->leftStick.x = lx;
      s->leftStick.y = ly;
   }
   if (rx != 0.0f || ry != 0.0f) {
      s->rightStick.x = rx;
      s->rightStick.y = ry;
   }
   return result;
}

/* Games and the Wii U Menu */
DECL_FUNCTION(int32_t, VPADRead, VPADChan chan, VPADStatus *buffers, uint32_t count, VPADReadError *outError)
{
   VPADReadError realError = VPAD_READ_SUCCESS;
   int32_t result = real_VPADRead(chan, buffers, count, &realError);

   if (chan == VPAD_CHAN_0 && sCfgEnabled && sPad.connected && buffers && count > 0) {
      if (!gamePadMode()) {
         
         checkGuideButton();
         enableProSupport();
         announceIfNeeded(sCfgProSlot - 1);
      } else {
         result = injectPad(buffers, result, &realError, false);
      }
   }
   if (outError) {
      *outError = realError;
   }
   return result;
}

WUPS_MUST_REPLACE(VPADRead, WUPS_LOADER_LIBRARY_VPAD, VPADRead);


DECL_FUNCTION(int32_t, VPADRead_HomeMenu, VPADChan chan, VPADStatus *buffers, uint32_t count, VPADReadError *outError)
{
   VPADReadError realError = VPAD_READ_SUCCESS;
   int32_t result = real_VPADRead_HomeMenu(chan, buffers, count, &realError);

   if (chan == VPAD_CHAN_0 && sCfgEnabled && gamePadMode() &&
       sPad.connected && buffers && count > 0) {
      result = injectPad(buffers, result, &realError, true);
   }
   if (outError) {
      *outError = realError;
   }
   return result;
}

WUPS_MUST_REPLACE_FOR_PROCESS(VPADRead_HomeMenu, WUPS_LOADER_LIBRARY_VPAD, VPADRead,
                              WUPS_FP_TARGET_PROCESS_HOME_MENU);



DECL_FUNCTION(int32_t, VPADControlMotor, VPADChan chan, uint8_t *pattern, uint8_t length)
{
   if (chan == VPAD_CHAN_0) {
      if (pattern && length) {
         if (length > 120) {
            length = 120;
         }
         sRumbleLen = 0;                          /* pause reader while we copy */
         memcpy(sRumblePattern, pattern, 15);
         sRumbleStart = OSGetTime();
         sRumbleLen   = length;
      } else {
         sRumbleLen = 0;
      }
   }
   return real_VPADControlMotor(chan, pattern, length);
}

DECL_FUNCTION(void, VPADStopMotor, VPADChan chan)
{
   if (chan == VPAD_CHAN_0) {
      sRumbleLen = 0;
   }
   real_VPADStopMotor(chan);
}

WUPS_MUST_REPLACE(VPADControlMotor, WUPS_LOADER_LIBRARY_VPAD, VPADControlMotor);
WUPS_MUST_REPLACE(VPADStopMotor, WUPS_LOADER_LIBRARY_VPAD, VPADStopMotor);



