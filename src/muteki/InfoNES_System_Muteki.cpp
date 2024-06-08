/*===================================================================*/
/*                                                                   */
/*  InfoNES_System_Muteki.cpp : Besta RTOS specific File             */
/*                                                                   */
/*  2001/05/18  InfoNES Project ( Sound is based on DarcNES )        */
/*                                                                   */
/*===================================================================*/

/*-------------------------------------------------------------------*/
/*  Include files                                                    */
/*-------------------------------------------------------------------*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <muteki/audio.h>
#include <muteki/datetime.h>
#include <muteki/devio.h>
#include <muteki/system.h>
#include <muteki/utf16.h>
#include <muteki/ui/common.h>
#include <muteki/ui/canvas.h>
#include <muteki/ui/event.h>
#include <muteki/ui/surface.h>
#include <muteki/ui/views/messagebox.h>
#include <muteki/ui/views/filepicker.h>

#include "../InfoNES.h"
#include "../InfoNES_System.h"
#include "../InfoNES_pAPU.h"

/*-------------------------------------------------------------------*/
/*  ROM image file information                                       */
/*-------------------------------------------------------------------*/

char szRomName[ 260 ];
char szSaveName[ 260 ];
int nSRAM_SaveFlag;

/*-------------------------------------------------------------------*/
/*  Constants ( Besta RTOS specific )                                */
/*-------------------------------------------------------------------*/

#define VERSION      "InfoNES v0.96J"
const char ROM_FILE_NAME[] = "rom.nes";

const key_press_event_config_t KEY_EVENT_CONFIG_DRAIN = {65535, 65535, 1};
const key_press_event_config_t KEY_EVENT_CONFIG_TURBO = {0, 0, 0};

/*-------------------------------------------------------------------*/
/*  Global Variables ( Besta RTOS specific )                         */
/*-------------------------------------------------------------------*/

/* Quit flag */
bool quit = false;
bool dis_active = false;

int last_millis = 0;
short blit_offset_x = 0;
short blit_offset_y = 0;
short pressing0 = 0, pressing1 = 0;
size_t xbound = 0;
size_t bufbase = 0;
short frame_advance_cnt = 0;

pcm_codec_context_t *pcmdesc = nullptr;
devio_descriptor_t *pcmdev = DEVIO_DESC_INVALID;

int pcm_sample_rate = -1;
short audio_buffer[0x1000] = {0};
size_t audio_pos = 0;

auto old_hold_cfg = key_press_event_config_t();

lcd_surface_t *IntermediateSurface = nullptr, *RealSurface = nullptr;

/* Palette data */
WORD NesPalette[ 64 ] =
{
  0x39ce, 0x1071, 0x0015, 0x2013, 0x440e, 0x5402, 0x5000, 0x3c20,
  0x20a0, 0x0100, 0x0140, 0x00e2, 0x0ceb, 0x0000, 0x0000, 0x0000,
  0x5ef7, 0x01dd, 0x10fd, 0x401e, 0x5c17, 0x700b, 0x6ca0, 0x6521,
  0x45c0, 0x0240, 0x02a0, 0x0247, 0x0211, 0x0000, 0x0000, 0x0000,
  0x7fff, 0x1eff, 0x2e5f, 0x223f, 0x79ff, 0x7dd6, 0x7dcc, 0x7e67,
  0x7ae7, 0x4342, 0x2769, 0x2ff3, 0x03bb, 0x0000, 0x0000, 0x0000,
  0x7fff, 0x579f, 0x635f, 0x6b3f, 0x7f1f, 0x7f1b, 0x7ef6, 0x7f75,
  0x7f94, 0x73f4, 0x57d7, 0x5bf9, 0x4ffe, 0x0000, 0x0000, 0x0000
};

/*-------------------------------------------------------------------*/
/*  Function prototypes ( Besta RTOS specific )                      */
/*-------------------------------------------------------------------*/

int LoadSRAM();
int SaveSRAM();

static void _ext_ticker();
static void _direct_input_sim_begin();
static void _direct_input_sim_end();
static void _drain_all_events();
static DWORD _map_pad_state(short key);
static DWORD _map_pad_state_system(short key);
static int sound_on();
static void sound_off();

/*===================================================================*/
/*                                                                   */
/*                main() : Application main                          */
/*                                                                   */
/*===================================================================*/

/* Application main */
int main(int argc, char **argv)
{
  // Boot with APU muted by default
  // TODO make this configurable once we get a UI
  APU_Mute = 1;

  filepicker_context_t ctx = {0};
  void *paths = FILEPICKER_CONTEXT_OUTPUT_ALLOC(calloc, 1, true);
  if (paths == NULL) {
    return 1;
  }
  auto *path = reinterpret_cast<UTF16 *>(calloc(260, 2));
  if (path == NULL) {
    free(paths);
    return 1;
  }

  ctx.paths = paths;
  ctx.ctx_size = sizeof(ctx);
  ctx.unk_0x30 = 0xffff;
  ctx.type_list = "NES ROM Files (*.nes)\0*.nes\0All Files (*.*)\0*.*\0\0\0";

  bool ret = _GetOpenFileName(&ctx);

  if (!ret || _GetNextFileName(&ctx, path) != 0) {
    free(paths);
    free(path);
    return 0;
  }

  // TODO get a proper wc2mb to work here
  for (size_t i = 0; i < sizeof(szRomName); i++) {
    if (path[i] > 0xff) {
      free(paths);
      free(path);
      return 1;
    }
    szRomName[i] = path[i] & 0xff;
  }

  free(paths);
  free(path);

  rgbSetBkColor(0);
  ClearScreen(false);

  blit_offset_x = (GetMaxScrX() + 1 - NES_DISP_WIDTH) / 2;
  blit_offset_y = (GetMaxScrY() + 1 - NES_DISP_HEIGHT) / 2;
  if (blit_offset_x < 0) {
    blit_offset_x = 0;
  }
  if (blit_offset_y < 0) {
    blit_offset_y = 0;
  }

  auto active_lcd = GetActiveLCD();
  if (active_lcd != nullptr) {
    RealSurface = GetActiveLCD()->surface;
    // TODO support and whitelist more buffer formats
    if (RealSurface != nullptr && RealSurface->depth == LCD_SURFACE_PIXFMT_XRGB &&
        RealSurface->width >= NES_DISP_WIDTH && RealSurface->height >= NES_DISP_HEIGHT) {
      // Fast blit possible (XRGB hardware buffer format).
      xbound = blit_offset_x + NES_DISP_WIDTH - 1;
      bufbase = blit_offset_x + blit_offset_y * RealSurface->width;
    } else {
      // Fast blit not possible. Falling back to safe blit.
      RealSurface = nullptr;
    }
  }

  if (RealSurface == nullptr) {
    // Allocate intermediate surface
    IntermediateSurface = reinterpret_cast<lcd_surface_t *>(
      malloc(GetImageSizeExt(NES_DISP_WIDTH, NES_DISP_HEIGHT, LCD_SURFACE_PIXFMT_XRGB))
    );
    if (IntermediateSurface == nullptr) {
      InfoNES_MessageBox("Failed to allocate memory for intermediate buffer.");
      return 1;
    }
    InitGraphic(IntermediateSurface, NES_DISP_WIDTH, NES_DISP_HEIGHT, LCD_SURFACE_PIXFMT_XRGB);
  }

  if (InfoNES_Load(szRomName) == 0) {
    LoadSRAM();
  } else {
    return 1;
  }

  _direct_input_sim_begin();
  InfoNES_Main();
  SaveSRAM();
  _direct_input_sim_end();

  if (IntermediateSurface != nullptr) {
    free(IntermediateSurface);
    IntermediateSurface = nullptr;
  }

  return 0;
}

/*===================================================================*/
/*                                                                   */
/*           LoadSRAM() : Load a SRAM                                */
/*                                                                   */
/*===================================================================*/
int LoadSRAM()
{
/*
 *  Load a SRAM
 *
 *  Return values
 *     0 : Normally
 *    -1 : SRAM data couldn't be read
 */

  FILE *fp;
  unsigned char pSrcBuf[ SRAM_SIZE ];
  unsigned char chData;
  unsigned char chTag;
  int nRunLen;
  int nDecoded;
  int nDecLen;
  int nIdx;

  // It doesn't need to save it
  nSRAM_SaveFlag = 0;

  // It is finished if the ROM doesn't have SRAM
  if ( !ROM_SRAM )
    return 0;

  // There is necessity to save it
  nSRAM_SaveFlag = 1;

  // The preparation of the SRAM file name
  strcpy( szSaveName, szRomName );
  strcpy( strrchr( szSaveName, '.' ) + 1, "srm" );

  /*-------------------------------------------------------------------*/
  /*  Read a SRAM data                                                 */
  /*-------------------------------------------------------------------*/

  // Open SRAM file
  fp = fopen( szSaveName, "rb" );
  if ( fp == NULL )
    return -1;

  // Read SRAM data
  fread( pSrcBuf, SRAM_SIZE, 1, fp );

  // Close SRAM file
  fclose( fp );

  /*-------------------------------------------------------------------*/
  /*  Extract a SRAM data                                              */
  /*-------------------------------------------------------------------*/

  nDecoded = 0;
  nDecLen = 0;

  chTag = pSrcBuf[ nDecoded++ ];

  while ( nDecLen < 8192 )
  {
    chData = pSrcBuf[ nDecoded++ ];

    if ( chData == chTag )
    {
      chData = pSrcBuf[ nDecoded++ ];
      nRunLen = pSrcBuf[ nDecoded++ ];
      for ( nIdx = 0; nIdx < nRunLen + 1; ++nIdx )
      {
        SRAM[ nDecLen++ ] = chData;
      }
    }
    else
    {
      SRAM[ nDecLen++ ] = chData;
    }
  }

  // Successful
  return 0;
}

/*===================================================================*/
/*                                                                   */
/*           SaveSRAM() : Save a SRAM                                */
/*                                                                   */
/*===================================================================*/
int SaveSRAM()
{
/*
 *  Save a SRAM
 *
 *  Return values
 *     0 : Normally
 *    -1 : SRAM data couldn't be written
 */

  FILE *fp;
  int nUsedTable[ 256 ];
  unsigned char chData;
  unsigned char chPrevData;
  unsigned char chTag;
  int nIdx;
  int nEncoded;
  int nEncLen;
  int nRunLen;
  unsigned char pDstBuf[ SRAM_SIZE ];

  if ( !nSRAM_SaveFlag )
    return 0;  // It doesn't need to save it

  /*-------------------------------------------------------------------*/
  /*  Compress a SRAM data                                             */
  /*-------------------------------------------------------------------*/

  memset( nUsedTable, 0, sizeof nUsedTable );

  for ( nIdx = 0; nIdx < SRAM_SIZE; ++nIdx )
  {
    ++nUsedTable[ SRAM[ nIdx++ ] ];
  }
  for ( nIdx = 1, chTag = 0; nIdx < 256; ++nIdx )
  {
    if ( nUsedTable[ nIdx ] < nUsedTable[ chTag ] )
      chTag = nIdx;
  }

  nEncoded = 0;
  nEncLen = 0;
  nRunLen = 1;

  pDstBuf[ nEncLen++ ] = chTag;

  chPrevData = SRAM[ nEncoded++ ];

  while ( nEncoded < SRAM_SIZE && nEncLen < SRAM_SIZE - 133 )
  {
    chData = SRAM[ nEncoded++ ];

    if ( chPrevData == chData && nRunLen < 256 )
      ++nRunLen;
    else
    {
      if ( nRunLen >= 4 || chPrevData == chTag )
      {
        pDstBuf[ nEncLen++ ] = chTag;
        pDstBuf[ nEncLen++ ] = chPrevData;
        pDstBuf[ nEncLen++ ] = nRunLen - 1;
      }
      else
      {
        for ( nIdx = 0; nIdx < nRunLen; ++nIdx )
          pDstBuf[ nEncLen++ ] = chPrevData;
      }

      chPrevData = chData;
      nRunLen = 1;
    }

  }
  if ( nRunLen >= 4 || chPrevData == chTag )
  {
    pDstBuf[ nEncLen++ ] = chTag;
    pDstBuf[ nEncLen++ ] = chPrevData;
    pDstBuf[ nEncLen++ ] = nRunLen - 1;
  }
  else
  {
    for ( nIdx = 0; nIdx < nRunLen; ++nIdx )
      pDstBuf[ nEncLen++ ] = chPrevData;
  }

  /*-------------------------------------------------------------------*/
  /*  Write a SRAM data                                                */
  /*-------------------------------------------------------------------*/

  // Open SRAM file
  fp = fopen( szSaveName, "wb" );
  if ( fp == NULL )
    return -1;

  // Write SRAM data
  fwrite( pDstBuf, nEncLen, 1, fp );

  // Close SRAM file
  fclose( fp );

  // Successful
  return 0;
}

static inline bool _test_events_no_shift(ui_event_t *uievent) {
    // Deactivate shift key because it may cause the keycode to change.
    // This means we need to handle shift behavior ourselves (if we really need it) but that's a fair tradeoff.
    SetShiftState(TOGGLE_KEY_INACTIVE);
    return TestPendEvent(uievent) || TestKeyEvent(uievent);
}

static void _ext_ticker() {
  static auto uievent = ui_event_t();
  bool hit = false;

  // TODO this still seem to lose track presses on BA110. Find out why.
  while (_test_events_no_shift(&uievent)) {
    hit = true;
    if (GetEvent(&uievent) && uievent.event_type == 0x10) {
      pressing0 = uievent.key_code0;
      pressing1 = uievent.key_code1;
    } else {
      ClearEvent(&uievent);
    }
  }

  if (!hit) {
    pressing0 = 0;
    pressing1 = 0;
  }
}

static void _drain_all_events() {
  auto uievent = ui_event_t();
  size_t silence_count = 0;
  while (silence_count < 60) {
    bool test = (TestPendEvent(&uievent) || TestKeyEvent(&uievent));
    if (test) {
      ClearAllEvents();
      silence_count = 0;
    }
    OSSleep(1);
    silence_count++;
  }
}

static void _direct_input_sim_begin() {
  if (!dis_active) {
    GetSysKeyState(&old_hold_cfg);
    SetTimer1IntHandler(&_ext_ticker, 3);
    SetSysKeyState(&KEY_EVENT_CONFIG_TURBO);
    dis_active = true;
  }
}

static void _direct_input_sim_end() {
  if (dis_active) {
    SetSysKeyState(&KEY_EVENT_CONFIG_DRAIN);
    SetTimer1IntHandler(NULL, 0);
    _drain_all_events();
    SetSysKeyState(&old_hold_cfg);
    dis_active = false;
  }
}

static inline DWORD _map_pad_state_system(short key) {
  switch (key) {
    case KEY_ESC:
      return PAD_SYS_QUIT;
    default:
      return 0;
  }
}

static inline DWORD _map_pad_state(short key) {
  switch (key) {
    case KEY_RIGHT:
      return 1 << 7;
    case KEY_LEFT:
      return 1 << 6;
    case KEY_DOWN:
      return 1 << 5;
    case KEY_UP:
      return 1 << 4;
    case KEY_S:
      return 1 << 3; // Start
    case KEY_A:
      return 1 << 2; // Select
    case KEY_Z:
      return 1 << 1; // B
    case KEY_X:
      return 1; // A
    default:
      return 0;
  }
}

static int sound_on() {
  sound_off();

  audio_pos = 0;

  pcmdesc = OpenPCMCodec(DIRECTION_OUT, pcm_sample_rate, FORMAT_PCM_MONO);
  if (pcmdesc == nullptr) {
    return 0;
  }

  pcmdev = CreateFile("\\\\?\\PCM", 0, 0, NULL, 3, 0, NULL);
  if (pcmdev == nullptr || pcmdev == DEVIO_DESC_INVALID) {
    ClosePCMCodec(pcmdesc);
    pcmdesc = nullptr;
    return 0;
  }

  return 1;
}

static void sound_off() {
  if (pcmdev != nullptr && pcmdev != DEVIO_DESC_INVALID) {
    CloseHandle(pcmdev);
    pcmdev = DEVIO_DESC_INVALID;
  }

  if (pcmdesc != nullptr) {
    ClosePCMCodec(pcmdesc);
    pcmdesc = nullptr;
  }
}

/*===================================================================*/
/*                                                                   */
/*                  InfoNES_Menu() : Menu screen                     */
/*                                                                   */
/*===================================================================*/
int InfoNES_Menu()
{
/*
 *  Menu screen
 *
 *  Return values
 *     0 : Normally
 *    -1 : Exit InfoNES
 */

  /* If terminated */
  if (quit)
  {
    return -1;
  }

  /* Nothing to do here */
  return 0;
}

/*===================================================================*/
/*                                                                   */
/*               InfoNES_ReadRom() : Read ROM image file             */
/*                                                                   */
/*===================================================================*/
int InfoNES_ReadRom( const char *pszFileName )
{
/*
 *  Read ROM image file
 *
 *  Parameters
 *    const char *pszFileName          (Read)
 *
 *  Return values
 *     0 : Normally
 *    -1 : Error
 */

  FILE *fp;

  /* Open ROM file */
  fp = fopen( pszFileName, "rb" );
  if ( fp == NULL )
    return -1;

  /* Read ROM Header */
  fread( &NesHeader, sizeof NesHeader, 1, fp );
  if ( memcmp( NesHeader.byID, "NES\x1a", 4 ) != 0 )
  {
    /* not .nes file */
    fclose( fp );
    return -1;
  }

  /* Clear SRAM */
  memset( SRAM, 0, SRAM_SIZE );

  /* If trainer presents Read Triner at 0x7000-0x71ff */
  if ( NesHeader.byInfo1 & 4 )
  {
    fread( &SRAM[ 0x1000 ], 512, 1, fp );
  }

  /* Allocate Memory for ROM Image */
  ROM = (BYTE *)malloc( NesHeader.byRomSize * 0x4000 );

  /* Read ROM Image */
  fread( ROM, 0x4000, NesHeader.byRomSize, fp );

  if ( NesHeader.byVRomSize > 0 )
  {
    /* Allocate Memory for VROM Image */
    VROM = (BYTE *)malloc( NesHeader.byVRomSize * 0x2000 );

    /* Read VROM Image */
    fread( VROM, 0x2000, NesHeader.byVRomSize, fp );
  }

  /* File close */
  fclose( fp );

  /* Successful */
  return 0;
}

/*===================================================================*/
/*                                                                   */
/*           InfoNES_ReleaseRom() : Release a memory for ROM         */
/*                                                                   */
/*===================================================================*/
void InfoNES_ReleaseRom()
{
/*
 *  Release a memory for ROM
 *
 */

  if ( ROM )
  {
    free( ROM );
    ROM = NULL;
  }

  if ( VROM )
  {
    free( VROM );
    VROM = NULL;
  }
}

/*===================================================================*/
/*                                                                   */
/*             InfoNES_MemoryCopy() : memcpy                         */
/*                                                                   */
/*===================================================================*/
void *InfoNES_MemoryCopy( void *dest, const void *src, int count )
{
/*
 *  memcpy
 *
 *  Parameters
 *    void *dest                       (Write)
 *      Points to the starting address of the copied block's destination
 *
 *    const void *src                  (Read)
 *      Points to the starting address of the block of memory to copy
 *
 *    int count                        (Read)
 *      Specifies the size, in bytes, of the block of memory to copy
 *
 *  Return values
 *    Pointer of destination
 */

  memcpy( dest, src, count );
  return dest;
}

/*===================================================================*/
/*                                                                   */
/*             InfoNES_MemorySet() : memset                          */
/*                                                                   */
/*===================================================================*/
void *InfoNES_MemorySet( void *dest, int c, int count )
{
/*
 *  memset
 *
 *  Parameters
 *    void *dest                       (Write)
 *      Points to the starting address of the block of memory to fill
 *
 *    int c                            (Read)
 *      Specifies the byte value with which to fill the memory block
 *
 *    int count                        (Read)
 *      Specifies the size, in bytes, of the block of memory to fill
 *
 *  Return values
 *    Pointer of destination
 */

  memset( dest, c, count);  
  return dest;
}

/*===================================================================*/
/*                                                                   */
/*      InfoNES_LoadFrame() :                                        */
/*           Transfer the contents of work frame on the screen       */
/*                                                                   */
/*===================================================================*/
void InfoNES_LoadFrame()
{
  datetime_t dt;

  if (RealSurface != nullptr) {
    auto fb = reinterpret_cast<int *>(RealSurface->buffer);
    auto xoff = blit_offset_x;
    auto lineoff = bufbase;
    auto bufoff = bufbase;
    for (size_t i = 0; i < NES_DISP_WIDTH * NES_DISP_HEIGHT; i++) {
      fb[bufoff] = (
        ((WorkFrame[i] & 0x001f) << 3) |
        ((WorkFrame[i] & 0x03e0) << 6) |
        ((WorkFrame[i] & 0x7c00) << 9) |
        0xff000000
      );
      if (xoff >= xbound) {
        xoff = blit_offset_x;
        lineoff += RealSurface->width;
        bufoff = lineoff;
      } else {
        xoff++;
        bufoff++;
      }
    }
  } else if (IntermediateSurface != nullptr) {
    auto fb = reinterpret_cast<int *>(IntermediateSurface->buffer);
    for (size_t i = 0; i < NES_DISP_WIDTH * NES_DISP_HEIGHT; i++) {
      fb[i] = (
        ((WorkFrame[i] & 0x001f) << 3) |
        ((WorkFrame[i] & 0x03e0) << 6) |
        ((WorkFrame[i] & 0x7c00) << 9)
      );
    }
    // TODO maybe use _BitBlt to write to the LCD buffer so we can center the image regardless of whether blit_offset_* is negative or not
    ShowGraphic(blit_offset_x, blit_offset_y, IntermediateSurface, BLIT_NONE);
  }

  // Calculate frame advance time
  short frame_advance = (frame_advance_cnt == 0) ? 16 : 17;
  frame_advance_cnt++;
  if (frame_advance_cnt >= 3) {
    frame_advance_cnt = 0;
  }

  GetSysTime(&dt);
  short elapsed_millis = (dt.millis >= last_millis) ? (dt.millis - last_millis) : (1000 + dt.millis - last_millis);
  short sleep_millis = frame_advance - elapsed_millis;

  if (sleep_millis > 0) {
    OSSleep(sleep_millis);
  }

  last_millis = dt.millis;
}

/*===================================================================*/
/*                                                                   */
/*             InfoNES_PadState() : Get a joypad state               */
/*                                                                   */
/*===================================================================*/
void InfoNES_PadState( DWORD *pdwPad1, DWORD *pdwPad2, DWORD *pdwSystem )
{
  static bool holding_m = false;
  *pdwPad1 = _map_pad_state(pressing0) | _map_pad_state(pressing1);
  *pdwSystem = _map_pad_state_system(pressing0) | _map_pad_state(pressing1);
  if (*pdwSystem & PAD_SYS_QUIT) {
    quit = true;
  }
  if ((pressing0 == KEY_M || pressing1 == KEY_M) && !holding_m) {
    holding_m = true;
    APU_Mute ^= 1;
    if (APU_Mute == 0) {
      sound_on();
    } else {
      sound_off();
    }
  } else if (pressing0 != KEY_M && pressing1 != KEY_M) {
    holding_m = false;
  }
}

/*===================================================================*/
/*                                                                   */
/*        InfoNES_SoundInit() : Sound Emulation Initialize           */
/*                                                                   */
/*===================================================================*/
void InfoNES_SoundInit( void ) 
{
  return;
}

/*===================================================================*/
/*                                                                   */
/*        InfoNES_SoundOpen() : Sound Open                           */
/*                                                                   */
/*===================================================================*/
int InfoNES_SoundOpen( int samples_per_sync, int sample_rate ) 
{
  (void) samples_per_sync;

  pcm_sample_rate = sample_rate;

  if (APU_Mute == 1) {
    return 1;
  }
  return sound_on();
}

/*===================================================================*/
/*                                                                   */
/*        InfoNES_SoundClose() : Sound Close                         */
/*                                                                   */
/*===================================================================*/
void InfoNES_SoundClose( void ) 
{
  sound_off();
}

/*===================================================================*/
/*                                                                   */
/*            InfoNES_SoundOutput() : Sound Output 5 Waves           */           
/*                                                                   */
/*===================================================================*/
void InfoNES_SoundOutput( int samples, BYTE *wave1, BYTE *wave2, BYTE *wave3, BYTE *wave4, BYTE *wave5 )
{
  size_t actual_size;

  for (int i = 0; i < samples; i++) {
    audio_buffer[audio_pos++] = (
      ((wave1[i] + wave2[i] + wave3[i] + wave4[i] + wave5[i]) / 5 - 128) * 256
    );
    if (audio_pos >= sizeof(audio_buffer) / sizeof(audio_buffer[0])) {
      if (pcmdev != nullptr && pcmdev != DEVIO_DESC_INVALID) {
        WriteFile(pcmdev, audio_buffer, sizeof(audio_buffer), &actual_size, nullptr);
      }
      audio_pos = 0;
    }
  }
}

/*===================================================================*/
/*                                                                   */
/*            InfoNES_Wait() : Wait Emulation if required            */
/*                                                                   */
/*===================================================================*/
void InfoNES_Wait() {}

/*===================================================================*/
/*                                                                   */
/*            InfoNES_MessageBox() : Print System Message            */
/*                                                                   */
/*===================================================================*/
void InfoNES_MessageBox( const char *pszMsg, ... )
{
  va_list args;
  bool toggle_dis = dis_active;

  auto pszErr = reinterpret_cast<char *>(calloc(1024, 1));
  if (pszErr == nullptr) {
    return;
  }

  auto pwszErr = reinterpret_cast<UTF16 *>(calloc(2048, 1));
  if (pwszErr == nullptr) {
    free(pszErr);
    return;
  }

  // Create the message body
  va_start( args, pszMsg );
  vsprintf( pszErr, pszMsg, args );  pszErr[ 1023 ] = '\0';
  va_end( args );

  ConvStrToUnicode(pszErr, pwszErr, MB_ENCODING_UTF8);

  if (toggle_dis) {
    _direct_input_sim_end();
  }

  MessageBox(pwszErr, MB_DEFAULT);

  if (toggle_dis) {
    _direct_input_sim_begin();
  }

  free(pszErr);
  free(pwszErr);
}

/*
 * End of InfoNES_System_Muteki.cpp
 */
