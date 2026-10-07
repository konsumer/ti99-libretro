// SPDX-License-Identifier: GPL-2.0-or-later
//
// libretro frontend for ti99sim, driving upstream/src/core: the TMS9900 CPU,
// the TMS9918A VDP and the console's memory map.
//
// The core hands the VDP raw VRAM (GetMemory) plus table addresses and the
// mode, so this file owns the rendering, the sound synthesis and the input
// mapping -- none of those live in the core, only in ti99sim's SDL frontend.
// The reference for the port is upstream/src/sdl/{tms9918a,tms9919,ti994a}-sdl.cpp.

#include <libretro.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.hpp"
#include "platform.hpp"
#include "ti994a.hpp"
#include "tms9918a.hpp"
#include "tms9919.hpp"
#include "tms9901.hpp"
#include "tms5220.hpp"
#include "cartridge.hpp"

// option.cpp is linked in, but its CLI entry point belongs to a frontend; the
// core has no command line, so satisfy the symbol with a no-op.
void PrintUsage () {}

#define VDP_W           256
#define VDP_H           192
#define AUDIO_RATE      44100
#define FRAME_CYCLES    ( CPU_SPEED_HZ / 60 )    // one VDP frame of CPU clocks
#define FRAME_SAMPLES   ( AUDIO_RATE / 60 )      // 44100/60 = 735

static retro_environment_t        g_env;
static retro_video_refresh_t      g_video;
static retro_audio_sample_batch_t g_audio;
static retro_input_poll_t         g_input_poll;
static retro_input_state_t        g_input_state;

static bool g_keyboard_supported;

static uint16_t g_frame [VDP_W * VDP_H];   // RGB565, libretro's format
static bool     g_frame_ready;
static bool     g_loaded;

// TMS9918A palette: the 15 TMS colours plus transparent, 8 bits per channel.
static const uint8_t kPalette [16][3] = {
    {   0,   0,   0 }, {   0,   0,   0 }, {  33, 200,  66 }, {  94, 220, 120 },
    {  84,  85, 237 }, { 125, 118, 252 }, { 212,  82,  77 }, {  66, 235, 245 },
    { 252,  85,  84 }, { 255, 121, 120 }, { 212, 193,  84 }, { 230, 206, 128 },
    {  33, 176,  59 }, { 201,  91, 186 }, { 204, 204, 204 }, { 255, 255, 255 },
};

static uint16_t g_palette565 [16];

static inline uint16_t rgb565 ( unsigned r, unsigned g, unsigned b )
{
    return ( uint16_t ) ((( r & 0xF8 ) << 8 ) | (( g & 0xFC ) << 3 ) | ( b >> 3 ));
}

// --- VDP rendering ---------------------------------------------------------
//
// A derived VDP: the console calls Refresh() from Retrace(), once per frame,
// after CheckSprites() has filled in the priority table. Rendering the whole
// frame each time is simpler than SDL's dirty-cell tracking and 256x192 is
// cheap. The protected members (m_Memory, m_Register, m_Mode, ...) are the
// same ones ti99sim's own renderer reads, so no core changes are needed.

class cRetroVDP : public cTMS9918A {
public:
    cRetroVDP ( int refreshRate ) : cTMS9918A ( refreshRate ) {}

    void Refresh ( bool force ) override
    {
        ( void ) force;
        RenderFrame ();
        g_frame_ready = true;
    }

    // cTMS9918A::SaveImage/LoadImage run the VRAM through ti99sim's RLE codec,
    // so the image has no fixed size and cannot back libretro's save states.
    // The state a running machine actually needs is written out verbatim here.
    void SaveState ( FILE *f )
    {
        fwrite ( &m_Address, sizeof ( m_Address ), 1, f );
        fwrite ( &m_Transfer, sizeof ( m_Transfer ), 1, f );
        fwrite ( &m_Shift, sizeof ( m_Shift ), 1, f );
        fwrite ( &m_Status, sizeof ( m_Status ), 1, f );
        fwrite ( &m_ReadAhead, sizeof ( m_ReadAhead ), 1, f );
        fwrite ( &m_Mode, sizeof ( m_Mode ), 1, f );
        fwrite ( m_Register, sizeof ( m_Register ), 1, f );
        fwrite ( m_MaxSprite, sizeof ( m_MaxSprite ), 1, f );
        fwrite ( &m_FifthSpriteIndex, sizeof ( m_FifthSpriteIndex ), 1, f );
        fwrite ( &m_InterruptLevel, sizeof ( m_InterruptLevel ), 1, f );
        fwrite ( &m_SpritesDirty, sizeof ( m_SpritesDirty ), 1, f );
        fwrite ( &m_SpritesRefreshed, sizeof ( m_SpritesRefreshed ), 1, f );
        fwrite ( &m_CoincidenceFlag, sizeof ( m_CoincidenceFlag ), 1, f );
        fwrite ( &m_FifthSpriteFlag, sizeof ( m_FifthSpriteFlag ), 1, f );
        fwrite ( m_Memory, 0x4000, 1, f );
    }

    void LoadState ( FILE *f )
    {
        fread ( &m_Address, sizeof ( m_Address ), 1, f );
        fread ( &m_Transfer, sizeof ( m_Transfer ), 1, f );
        fread ( &m_Shift, sizeof ( m_Shift ), 1, f );
        fread ( &m_Status, sizeof ( m_Status ), 1, f );
        fread ( &m_ReadAhead, sizeof ( m_ReadAhead ), 1, f );
        fread ( &m_Mode, sizeof ( m_Mode ), 1, f );
        fread ( m_Register, sizeof ( m_Register ), 1, f );
        fread ( m_MaxSprite, sizeof ( m_MaxSprite ), 1, f );
        fread ( &m_FifthSpriteIndex, sizeof ( m_FifthSpriteIndex ), 1, f );
        fread ( &m_InterruptLevel, sizeof ( m_InterruptLevel ), 1, f );
        fread ( &m_SpritesDirty, sizeof ( m_SpritesDirty ), 1, f );
        fread ( &m_SpritesRefreshed, sizeof ( m_SpritesRefreshed ), 1, f );
        fread ( &m_CoincidenceFlag, sizeof ( m_CoincidenceFlag ), 1, f );
        fread ( &m_FifthSpriteFlag, sizeof ( m_FifthSpriteFlag ), 1, f );
        fread ( m_Memory, 0x4000, 1, f );

        RebuildTables ();
    }

private:

    // The register -> table pointers cTMS9918A::WriteRegister derives, redone
    // here so restoring a state cannot trip WriteRegister's side effects (it
    // can flip the 16 KiB addressing or signal a PIC interrupt).
    void RebuildTables ()
    {
        int mode = m_Mode & ( VDP_M3 | VDP_M2 | VDP_M1 );

        m_ImageTableSize   = ( m_Register [1] & VDP_MODE_1_BIT ) ? 0x03C0 : 0x0300;
        m_ColorTableSize   = ( mode & VDP_M3 ) ? 0x1800 : 0x0020;
        m_PatternTableSize = ( mode & VDP_M3 ) ? 0x1800 : 0x0800;

        int colorOffset   = ( mode & VDP_M3 ) ? (( m_Register [3] & 0x80 ) ? 0x2000 : 0 )
                                              : m_Register [3] * 0x0040;
        int patternOffset = ( mode & VDP_M3 ) ? (( m_Register [4] & 0x04 ) ? 0x2000 : 0 )
                                              : ( m_Register [4] & 0x07 ) * 0x0800;

        m_ImageTable      = ( sScreenImage * )       &m_Memory [( m_Register [2] & 0x0F ) * 0x0400];
        m_ColorTable      = ( sColorTable * )        &m_Memory [colorOffset];
        m_PatternTable    = ( sPatternDescriptor * ) &m_Memory [patternOffset];
        m_SpriteAttrTable = ( sSpriteAttribute * )   &m_Memory [( m_Register [5] & 0x7F ) * 0x0080];
        m_SpriteDescTable = ( sSpriteDescriptor * )  &m_Memory [( m_Register [6] & 0x07 ) * 0x0800];
    }

    inline uint8_t vrd ( int a ) const { return m_Memory [a & 0x3FFF]; }
    inline const uint8_t *vptr ( int a ) const { return m_Memory + ( a & 0x3FFF ); }

    inline void put ( int x, int y, uint16_t colour )
    {
        if (( unsigned ) x < VDP_W && ( unsigned ) y < VDP_H )
            g_frame [y * VDP_W + x] = colour;
    }

    // Palette index 0 is transparent in the graphics modes and shows the
    // background colour -- ti99sim's SDL renderer remaps it the same way.
    inline uint16_t colour ( unsigned index, uint8_t bg ) const
    {
        return g_palette565 [index == 0 ? bg : index];
    }

    void DrawText ( uint8_t bg )
    {
        int image = GetImageTable ();
        int pattern = GetPatternTable ();
        uint8_t fore = m_Register [7] >> 4;
        uint8_t back = m_Register [7] & 0x0F;

        for ( int row = 0; row < 24; row++ ) {
            for ( int col = 0; col < 40; col++ ) {
                int ch = vrd ( image + row * 40 + col );
                const uint8_t *p = vptr ( pattern + ch * 8 );
                for ( int y = 0; y < 8; y++ ) {
                    uint8_t bits = p [y];
                    for ( int x = 0; x < 6; x++ ) {
                        int on = ( bits >> ( 7 - x )) & 1;
                        put ( 8 + col * 6 + x, row * 8 + y, colour ( on ? fore : back, bg ));
                    }
                }
            }
        }
    }

    void DrawGraphics1 ( uint8_t bg )
    {
        int image = GetImageTable ();
        int pattern = GetPatternTable ();
        int colourTable = GetColorTable ();

        for ( int row = 0; row < 24; row++ ) {
            for ( int col = 0; col < 32; col++ ) {
                int ch = vrd ( image + row * 32 + col );
                uint8_t attr = vrd ( colourTable + ch / 8 );
                uint8_t fore = attr >> 4;
                uint8_t back = attr & 0x0F;
                const uint8_t *p = vptr ( pattern + ch * 8 );
                for ( int y = 0; y < 8; y++ ) {
                    uint8_t bits = p [y];
                    for ( int x = 0; x < 8; x++ ) {
                        int on = ( bits >> ( 7 - x )) & 1;
                        put ( col * 8 + x, row * 8 + y, colour ( on ? fore : back, bg ));
                    }
                }
            }
        }
    }

    void DrawGraphics2 ( uint8_t bg )
    {
        int image = GetImageTable ();
        int pattern = GetPatternTable ();
        int colourTable = GetColorTable ();

        // The image table is 0x300 bytes: three 256-cell banks, each selecting
        // the matching 0x800-byte bank of the pattern/colour tables.
        for ( int i = 0; i < 0x300; i++ ) {
            int ch = ( i & 0xFF00 ) + vrd ( image + i );
            int x = ( i % 32 ) * 8;
            int y = ( i / 32 ) * 8;
            for ( int row = 0; row < 8; row++ ) {
                uint8_t attr = vrd ( colourTable + ch * 8 + row );
                uint8_t bits = vrd ( pattern + ch * 8 + row );
                uint8_t fore = attr >> 4;
                uint8_t back = attr & 0x0F;
                for ( int px = 0; px < 8; px++ ) {
                    int on = ( bits >> ( 7 - px )) & 1;
                    put ( x + px, y + row, colour ( on ? fore : back, bg ));
                }
            }
        }
    }

    void DrawMultiColor ( uint8_t bg )
    {
        int image = GetImageTable ();
        int pattern = GetPatternTable ();
        uint8_t back = m_Register [7] & 0x0F;

        for ( int row = 0; row < 24; row++ ) {
            for ( int col = 0; col < 32; col++ ) {
                int ch = vrd ( image + row * 32 + col );
                const uint8_t *p = vptr ( pattern + ch * 8 + ( row & 3 ) * 2 );
                for ( int i = 0; i < 2; i++ ) {
                    uint8_t b = p [i];
                    int left = b >> 4;
                    int right = b & 0x0F;
                    if ( left == 0 ) left = back;
                    if ( right == 0 ) right = back;
                    for ( int y = 0; y < 4; y++ ) {
                        for ( int x = 0; x < 4; x++ ) {
                            put ( col * 8 + x, row * 8 + i * 4 + y, g_palette565 [left] );
                            put ( col * 8 + 4 + x, row * 8 + i * 4 + y, g_palette565 [right] );
                        }
                    }
                }
            }
        }
    }

    void DrawSprite ( int index, int attr, int desc )
    {
        int entry = attr + index * 4;
        int posY  = vrd ( entry );
        int posX  = vrd ( entry + 1 );
        int patIndex = vrd ( entry + 2 );
        int attrByte = vrd ( entry + 3 );

        int spriteColour = attrByte & 0x0F;
        if ( spriteColour == 0 )
            return;

        int count = ( m_Register [1] & VDP_SPRITE_SIZE ) ? 4 : 1;
        int size  = ( m_Register [1] & VDP_SPRITE_MAGNIFY ) ? 16 : 8;

        for ( int i = 0; i < count; i++ ) {
            int x = posX + ( i / 2 ) * size;
            if ( attrByte & 0x80 )
                x -= 32;                       // early clock
            if ( x >= VDP_W )
                continue;

            int row = ( posY + 1 + ( i % 2 ) * size ) & 0xFF;
            const uint8_t *pattern = vptr ( desc + (( patIndex + i ) % 256 ) * 8 );

            for ( int y = 0; y < size; y++, row++ ) {
                int r = row & 0xFF;
                if ( r < VDP_H && index <= m_MaxSprite [r] ) {
                    uint8_t bits = pattern [0];
                    for ( int px = 0, col = x; px < size; px++, col++ ) {
                        if ( col >= VDP_W )
                            break;
                        if (( bits & 0x80 ) && col >= 0 )
                            g_frame [r * VDP_W + col] = g_palette565 [spriteColour];
                        if ( size == 8 || ( px & 1 ))
                            bits = ( uint8_t ) ( bits << 1 );
                    }
                }
                if ( size == 8 || ( y & 1 ))
                    pattern++;
            }
        }
    }

    void DrawSprites ()
    {
        int attr = GetSpriteAttrTable ();
        int desc = GetSpriteDescTable ();

        int i;
        for ( i = 0; i < 32; i++ ) {
            if ( vrd ( attr + i * 4 ) == 0xD0 )
                break;
        }
        // Draw in reverse so a lower numbered sprite ends up on top.
        while ( --i >= 0 )
            DrawSprite ( i, attr, desc );
    }

    void RenderFrame ()
    {
        uint8_t bg = m_Register [7] & 0x0F;
        uint16_t blank = g_palette565 [bg];
        for ( int i = 0; i < VDP_W * VDP_H; i++ )
            g_frame [i] = blank;

        if ( BlankEnabled ())
            return;

        int mode = GetMode ();

        if ( mode & VDP_M3 ) {
            DrawGraphics2 ( bg );
        } else if ( mode & VDP_M2 ) {
            DrawMultiColor ( bg );
        } else if ( mode & VDP_M1 ) {
            DrawText ( bg );
            return;                            // no sprites in text mode
        } else {
            DrawGraphics1 ( bg );
        }

        DrawSprites ();
    }
};

// --- Sound -----------------------------------------------------------------
//
// ti99sim's SN76489 sample generator lives in its SDL frontend, not in
// cTMS9919; this reproduces it (4 square-wave voices, one shared noise shift
// register) and renders a frame's worth of samples on demand.

class cRetroTMS9919 : public cTMS9919 {
public:

    cRetroTMS9919 () :
        m_ShiftRegister ( NOISE_RESET ),
        m_NoiseGenerator ( NOISE_WHITE_GENERATOR )
    {
        float volume = 128.0f / 4.0f;
        for ( int i = 0; i < 15; i++ ) {
            m_Volume [i] = ( int ) volume;
            volume /= 1.258925412f;            // -2 dB per step
        }
        m_Volume [15] = 0;

        memset ( m_Info, 0, sizeof ( m_Info ));

        // Prime the noise channel the way the SDL frontend does, so the first
        // SetNoise() sees a change and sets up its voice.
        NOISE_COLOR_E color = m_NoiseColor;
        int type = m_NoiseType;
        m_NoiseColor = ( NOISE_COLOR_E ) -1;
        m_NoiseType = -1;
        cRetroTMS9919::SetNoise ( color, type );
    }

    void Generate ( int16_t *out, int count )
    {
        int mix [FRAME_SAMPLES + 1];
        if ( count > FRAME_SAMPLES ) count = FRAME_SAMPLES;
        memset ( mix, 0, sizeof ( int ) * count );

        for ( int i = 0; i < 4; i++ ) {
            sVoice *info = &m_Info [i];
            if ( m_Attenuation [i] == 15 || info->period < 1.0f )
                continue;

            int left = count, j = 0;
            do {
                int n = ( info->toggle < left ) ? ( int ) info->toggle : left;
                left -= n;
                info->toggle -= n;
                while ( n-- ) mix [j++] += info->setting;

                if ( info->toggle < 1.0f ) {
                    info->toggle += info->period;
                    if ( i < 3 ) {
                        info->setting = -info->setting;
                    } else {
                        if ( m_ShiftRegister & 1 ) {
                            m_ShiftRegister ^= m_NoiseGenerator;
                            if ( m_ShiftRegister == 0 )
                                m_ShiftRegister = NOISE_RESET;
                            info->setting = -info->setting;
                        }
                        m_ShiftRegister >>= 1;
                    }
                }
            } while ( left > 0 );
        }

        for ( int i = 0; i < count; i++ ) {
            int v = mix [i];                    // 8-bit unsigned, 128 = silence
            if ( v < -128 ) v = -128;
            if ( v > 127 ) v = 127;
            int16_t s = ( int16_t ) ( v << 8 );
            out [i * 2 + 0] = s;
            out [i * 2 + 1] = s;
        }
    }

    void SaveState ( FILE *f )
    {
        fwrite ( m_Frequency, sizeof ( m_Frequency ), 1, f );
        fwrite ( m_Attenuation, sizeof ( m_Attenuation ), 1, f );
        fwrite ( &m_NoiseColor, sizeof ( m_NoiseColor ), 1, f );
        fwrite ( &m_NoiseType, sizeof ( m_NoiseType ), 1, f );
        fwrite ( &m_LastData, sizeof ( m_LastData ), 1, f );
        fwrite ( m_Info, sizeof ( m_Info ), 1, f );
        fwrite ( &m_ShiftRegister, sizeof ( m_ShiftRegister ), 1, f );
        fwrite ( &m_NoiseGenerator, sizeof ( m_NoiseGenerator ), 1, f );
    }

    void LoadState ( FILE *f )
    {
        fread ( m_Frequency, sizeof ( m_Frequency ), 1, f );
        fread ( m_Attenuation, sizeof ( m_Attenuation ), 1, f );
        fread ( &m_NoiseColor, sizeof ( m_NoiseColor ), 1, f );
        fread ( &m_NoiseType, sizeof ( m_NoiseType ), 1, f );
        fread ( &m_LastData, sizeof ( m_LastData ), 1, f );
        fread ( m_Info, sizeof ( m_Info ), 1, f );
        fread ( &m_ShiftRegister, sizeof ( m_ShiftRegister ), 1, f );
        fread ( &m_NoiseGenerator, sizeof ( m_NoiseGenerator ), 1, f );
    }

protected:

    void SetNoise ( NOISE_COLOR_E color, int type ) override
    {
        if ( color == m_NoiseColor && type == m_NoiseType )
            return;

        bool reset = ( color != m_NoiseColor ) ? true : false;

        cTMS9919::SetNoise ( color, type );

        if ( reset )
            m_ShiftRegister = NOISE_RESET;
        m_NoiseGenerator = ( color == NOISE_WHITE ) ? NOISE_WHITE_GENERATOR : NOISE_PERIODIC_GENERATOR;

        sVoice *info = &m_Info [3];
        if ( m_Frequency [3] != 0 ) {
            int volume = m_Volume [m_Attenuation [3]];
            info->period = ( float ) AUDIO_RATE / ( float ) m_Frequency [3];
            info->setting = ( info->setting > 0 ) ? volume : -volume;
        } else {
            info->period = 0.0f;
        }
    }

    void SetFrequency ( int tone, int freq ) override
    {
        if ( freq == m_Frequency [tone] )
            return;

        cTMS9919::SetFrequency ( tone, freq );

        sVoice *info = &m_Info [tone];
        if ( freq < AUDIO_RATE / 2 && freq != 0 ) {
            int volume = m_Volume [m_Attenuation [tone]];
            info->period = ( float ) AUDIO_RATE / ( float ) freq / 2.0f;
            info->setting = ( info->setting > 0 ) ? volume : -volume;
        } else {
            info->period = ( float ) FRAME_SAMPLES;
            info->setting = 0;
        }

        // The noise channel can be clocked from voice 2's frequency.
        if ( tone == 2 && m_NoiseType == 3 ) {
            m_Frequency [3] = m_Frequency [2];
            info = &m_Info [3];
            if ( m_Frequency [3] != 0 ) {
                int volume = m_Volume [m_Attenuation [3]];
                info->period = ( float ) AUDIO_RATE / ( float ) m_Frequency [3];
                info->setting = ( info->setting > 0 ) ? volume : -volume;
            } else {
                info->period = 0.0f;
            }
        }
    }

    void SetAttenuation ( int tone, int atten ) override
    {
        if ( atten == m_Attenuation [tone] )
            return;

        cTMS9919::SetAttenuation ( tone, atten );

        sVoice *info = &m_Info [tone];
        int volume = m_Volume [m_Attenuation [tone]];
        info->setting = ( info->setting > 0 ) ? volume : -volume;
    }

private:

    enum {
        NOISE_RESET              = 0x00F35,
        NOISE_WHITE_GENERATOR    = 0x12000,
        NOISE_PERIODIC_GENERATOR = 0x08000
    };

    struct sVoice {
        float  period;      // samples per half cycle
        float  toggle;      // samples until the next edge
        int    setting;     // current signed amplitude
    };

    sVoice  m_Info [4];
    int     m_Volume [16];
    int     m_ShiftRegister;
    int     m_NoiseGenerator;
};

// --- Console ---------------------------------------------------------------

// ti99sim drives the VDP retrace from a CPU timer hook; the hook is registered
// by cTI994A and reached through a subclass because the members are protected.
// The same subclass exposes the console state the CPU/VDP/PIC images do not
// cover, for save states.
class cRetroConsole : public cTI994A {
public:
    cRetroConsole ( cCartridge *console, cTMS9918A *vdp, cTMS9919 *sound, cTMS5220 *speech )
        : cTI994A ( console, vdp, sound, speech ) {}

    cTMS9901 *PIC () { return m_PIC; }

    void SaveState ( FILE *f )
    {
        UINT32 lastRetrace = m_CPU->GetClocks () - m_LastRetrace;
        fwrite ( &m_GromAddress, sizeof ( m_GromAddress ), 1, f );
        fwrite ( &m_GromLastInstruction, sizeof ( m_GromLastInstruction ), 1, f );
        fwrite ( &m_GromReadShift, sizeof ( m_GromReadShift ), 1, f );
        fwrite ( &m_GromWriteShift, sizeof ( m_GromWriteShift ), 1, f );
        fwrite ( &m_GromCounter, sizeof ( m_GromCounter ), 1, f );
        fwrite ( &lastRetrace, sizeof ( lastRetrace ), 1, f );
    }

    void LoadState ( FILE *f )
    {
        UINT32 lastRetrace = 0;
        fread ( &m_GromAddress, sizeof ( m_GromAddress ), 1, f );
        fread ( &m_GromLastInstruction, sizeof ( m_GromLastInstruction ), 1, f );
        fread ( &m_GromReadShift, sizeof ( m_GromReadShift ), 1, f );
        fread ( &m_GromWriteShift, sizeof ( m_GromWriteShift ), 1, f );
        fread ( &m_GromCounter, sizeof ( m_GromCounter ), 1, f );
        fread ( &lastRetrace, sizeof ( lastRetrace ), 1, f );
        m_LastRetrace = m_CPU->GetClocks () - lastRetrace;
        m_GromPtr = &m_GromMemory [ m_GromAddress ];
    }
};

static cRetroConsole  *g_console;
static cRetroVDP      *g_vdp;
static cRetroTMS9919  *g_sound;
static cCartridge     *g_cart;

// Previous-key state for edge detection, kept across frames.
static bool g_prev [512];
static bool g_prev_joy [RETRO_DEVICE_ID_JOYPAD_R3 + 1];

// Size of a save state, measured once from the fixed-size component images.
static size_t g_state_size;

// --- libretro API ----------------------------------------------------------

static const char *system_directory ( void )
{
    const char *dir = NULL;

    if ( g_env && g_env ( RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir ) && dir )
        return dir;
    return ".";
}

unsigned retro_api_version ( void ) { return RETRO_API_VERSION; }

static const struct retro_input_descriptor g_input_descriptors [] = {
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "Up (FCTN+E)" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "Down (FCTN+X)" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "Left (FCTN+S)" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right (FCTN+D)" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Enter" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Space" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Erase (FCTN+S)" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Next field (FCTN+7)" },
    { 0, 0, 0, 0, NULL },
};

void retro_set_environment ( retro_environment_t cb )
{
    g_env = cb;

    if ( cb ) {
        enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
        cb ( RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt );
        cb ( RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, ( void * ) g_input_descriptors );

        uint64_t caps = 0;
        if ( cb ( RETRO_ENVIRONMENT_GET_INPUT_DEVICE_CAPABILITIES, &caps ))
            g_keyboard_supported = ( caps & ( 1 << RETRO_DEVICE_KEYBOARD )) != 0;
    }
}
void retro_set_video_refresh ( retro_video_refresh_t cb ) { g_video = cb; }
void retro_set_audio_sample ( retro_audio_sample_t cb ) { ( void ) cb; }
void retro_set_audio_sample_batch ( retro_audio_sample_batch_t cb ) { g_audio = cb; }
void retro_set_input_poll ( retro_input_poll_t cb ) { g_input_poll = cb; }
void retro_set_input_state ( retro_input_state_t cb ) { g_input_state = cb; }

void retro_init ( void )
{
    for ( int i = 0; i < 16; i++ )
        g_palette565 [i] = rgb565 ( kPalette [i][0], kPalette [i][1], kPalette [i][2] );
}

// Tear the machine down. g_console owns the VDP, the sound chip and the
// console ROM cartridge; g_cart is only referenced by the console.
static void shutdown_game ( void )
{
    delete g_cart;     g_cart = NULL;
    delete g_console;  g_console = NULL;
    g_vdp = NULL;
    g_sound = NULL;
    g_loaded = false;
    g_state_size = 0;
}

void retro_deinit ( void )
{
    shutdown_game ();
}

void retro_get_system_info ( struct retro_system_info *info )
{
    memset ( info, 0, sizeof ( *info ));
    info->library_name     = "TI-99/4A (ti99sim)";
    info->library_version  = "1.0";
    info->valid_extensions = "ctg|bin|g";
    info->need_fullpath    = true;
    info->block_extract    = false;
}

void retro_get_system_av_info ( struct retro_system_av_info *info )
{
    memset ( info, 0, sizeof ( *info ));
    info->geometry.base_width   = VDP_W;
    info->geometry.base_height  = VDP_H;
    info->geometry.max_width    = VDP_W;
    info->geometry.max_height   = VDP_H;
    info->geometry.aspect_ratio = 4.0 / 3.0;
    info->timing.fps            = 60.0;
    info->timing.sample_rate    = AUDIO_RATE;
}

// --- content loading -------------------------------------------------------

static bool read_file ( const char *path, uint8_t *dst, size_t max, size_t *got )
{
    FILE *f = fopen ( path, "rb" );
    size_t n;

    if ( ! f )
        return false;
    n = fread ( dst, 1, max, f );
    fclose ( f );
    if ( got )
        *got = n;
    return n > 0;
}

// Build the console's own ROM cartridge from the raw 8 KiB CPU ROM and the
// 24 KiB of console GROM: CPU regions 0 and 1 (0x0000-0x1FFF) and GROM banks
// 0-2 (0x0000-0x5FFF), matching the system map in ti994a.cpp.
//
// The console's two 4 KiB mask ROMs hold the even- and odd-address bytes, so a
// raw dump concatenates the chips and the bytes have to be interleaved back
// into one 8 KiB image. Without this the reset vector reads >8309 instead of
// >0024 and the CPU executes scratchpad garbage.
static cCartridge *load_console_rom ( const char *dir )
{
    uint8_t raw [0x2000];
    uint8_t rom [0x2000];
    uint8_t grom [0x6000];
    char path [1024];
    size_t rawLen = 0, gromLen = 0;

    snprintf ( path, sizeof ( path ), "%s/994aROM.bin", dir );
    if ( ! read_file ( path, raw, sizeof ( raw ), &rawLen )) {
        fprintf ( stderr, "ti99: missing %s\n", path );
        return NULL;
    }

    if ( rawLen >= 0x2000 ) {
        for ( int k = 0; k < 0x1000; k++ ) {
            rom [2 * k]     = raw [k];
            rom [2 * k + 1] = raw [0x1000 + k];
        }
    } else {
        memcpy ( rom, raw, rawLen );
        memset ( rom + rawLen, 0, sizeof ( rom ) - rawLen );
    }

    snprintf ( path, sizeof ( path ), "%s/994aGROM.bin", dir );
    if ( ! read_file ( path, grom, sizeof ( grom ), &gromLen )) {
        fprintf ( stderr, "ti99: missing %s\n", path );
        return NULL;
    }

    cCartridge *console = new cCartridge ( NULL );

    for ( int i = 0; i < 2; i++ ) {
        sMemoryRegion *region = &console->CpuMemory [i];
        region->NumBanks = 1;
        region->Bank [0].Type = BANK_ROM;
        region->Bank [0].Data = new UINT8 [ROM_BANK_SIZE];
        memcpy ( region->Bank [0].Data, rom + ( size_t ) i * ROM_BANK_SIZE, ROM_BANK_SIZE );
    }

    // CPU region 8 (0x8000-0x8FFF) is the console's scratch-pad RAM: without
    // it 0x8300-0x83FF stays marked ROM and every register write is discarded.
    // Regions 2/3 (>2000->3FFF) and 10-15 (>A000->FFFF) are the 32 KiB RAM
    // expansion, which the "-32KB" cartridge images require.
    {
        static const int ramRegions [] = { 2, 3, 8, 10, 11, 12, 13, 14, 15 };

        for ( unsigned i = 0; i < sizeof ( ramRegions ) / sizeof ( ramRegions [0] ); i++ ) {
            sMemoryRegion *region = &console->CpuMemory [ramRegions [i]];
            region->NumBanks = 1;
            region->Bank [0].Type = BANK_RAM;
            region->Bank [0].Data = new UINT8 [ROM_BANK_SIZE];
            memset ( region->Bank [0].Data, 0, ROM_BANK_SIZE );
        }
    }

    for ( int i = 0; i < 3; i++ ) {
        sMemoryRegion *region = &console->GromMemory [i];
        region->NumBanks = 1;
        region->Bank [0].Type = BANK_ROM;
        region->Bank [0].Data = new UINT8 [GROM_BANK_SIZE];
        memset ( region->Bank [0].Data, 0, GROM_BANK_SIZE );
        size_t offset = ( size_t ) i * GROM_BANK_SIZE;
        if ( offset < gromLen ) {
            size_t n = gromLen - offset;
            if ( n > GROM_BANK_SIZE ) n = GROM_BANK_SIZE;
            memcpy ( region->Bank [0].Data, grom + offset, n );
        }
    }

    return console;
}

// A .ctg file is ti99sim's own cartridge image. A raw .bin is a cartridge ROM:
// the ROM-only images used by the FlashROM 99 / FinalGROM 99 map up to 32 KiB
// as 8 KiB banks at 0x6000-0x7FFF, selected by the address written inside that
// window -- which is what cTI994A::BankSwitch implements, with region 6
// carrying each bank's low 4 KiB and region 7 its high 4 KiB, so both switch
// together. A raw .g is a GROM image, which sits above the console's own GROMs
// and so starts at GROM base 3.
static cCartridge *load_cartridge ( const char *path )
{
    const char *ext = strrchr ( path, '.' );

    if ( ext && strcmp ( ext, ".ctg" ) == 0 ) {
        cCartridge *cart = new cCartridge ( path );
        if ( cart->IsValid ())
            return cart;
        delete cart;
        return NULL;
    }

    FILE *f = fopen ( path, "rb" );
    if ( ! f )
        return NULL;
    fseek ( f, 0, SEEK_END );
    long size = ftell ( f );
    if ( size <= 0 ) {
        fclose ( f );
        return NULL;
    }

    cCartridge *cart = new cCartridge ( NULL );

    if ( ext && ( ext [1] == 'g' || ext [1] == 'G' ) && ext [2] == '\0' ) {
        int banks = ( int ) (( size + GROM_BANK_SIZE - 1 ) / GROM_BANK_SIZE );
        if ( banks > 8 - 3 ) banks = 8 - 3;
        for ( int i = 0; i < banks; i++ ) {
            sMemoryRegion *region = &cart->GromMemory [3 + i];
            region->NumBanks = 1;
            region->Bank [0].Type = BANK_ROM;
            region->Bank [0].Data = new UINT8 [GROM_BANK_SIZE];
            memset ( region->Bank [0].Data, 0, GROM_BANK_SIZE );
            fseek ( f, ( long ) i * GROM_BANK_SIZE, SEEK_SET );
            fread ( region->Bank [0].Data, 1, GROM_BANK_SIZE, f );
        }
    } else {
        int banks = ( int ) (( size + 0x1FFF ) / 0x2000 );
        if ( banks > 4 ) banks = 4;

        for ( int i = 0; i < 2; i++ ) {
            sMemoryRegion *region = &cart->CpuMemory [6 + i];
            region->NumBanks = banks;
            for ( int b = 0; b < banks; b++ ) {
                region->Bank [b].Type = BANK_ROM;
                region->Bank [b].Data = new UINT8 [ROM_BANK_SIZE];
                memset ( region->Bank [b].Data, 0xFF, ROM_BANK_SIZE );
                long offset = ( long ) b * 0x2000 + i * ROM_BANK_SIZE;
                if ( offset < size ) {
                    fseek ( f, offset, SEEK_SET );
                    fread ( region->Bank [b].Data, 1, ROM_BANK_SIZE, f );
                }
            }
        }
    }

    fclose ( f );
    return cart;
}

bool retro_load_game ( const struct retro_game_info *info )
{
    cCartridge *console;

    console = load_console_rom ( system_directory ());
    if ( ! console )
        return false;

    shutdown_game ();                      // in case a previous load lingered
    memset ( g_prev, 0, sizeof ( g_prev ));
    memset ( g_prev_joy, 0, sizeof ( g_prev_joy ));
    g_frame_ready = false;

    g_vdp     = new cRetroVDP ( 60 );
    g_sound   = new cRetroTMS9919 ();
    g_console = new cRetroConsole ( console, g_vdp, g_sound, NULL );

    if ( info && info->path && *info->path ) {
        g_cart = load_cartridge ( info->path );
        if ( ! g_cart ) {
            fprintf ( stderr, "ti99: cannot load cartridge %s\n", info->path );
            shutdown_game ();
            return false;
        }
        g_console->InsertCartridge ( g_cart, true );
    }

    g_console->Reset ();
    g_loaded = true;
    return true;
}

void retro_unload_game ( void ) { shutdown_game (); }

void retro_reset ( void )
{
    if ( g_loaded )
        g_console->Reset ();
}

// --- input -----------------------------------------------------------------

// One libretro key (or pad button) and the TI virtual key(s) it drives. The
// `sym` passed to the PIC is the libretro id, so press/release stays paired.
struct sKeyBinding {
    unsigned        id;
    VIRTUAL_KEY_E   vk1;
    VIRTUAL_KEY_E   vk2;
};

// Pad buttons get their own symbol space (400+) so they never clash with a
// keyboard key id used as a symbol.
#define JOY_SYM( b )  ( 400 + ( b ))

static const sKeyBinding kKeyboard [] = {
    { RETROK_RETURN,       VK_ENTER,     VK_NONE },
    { RETROK_SPACE,        VK_SPACE,     VK_NONE },
    { RETROK_COMMA,        VK_COMMA,     VK_NONE },
    { RETROK_PERIOD,       VK_PERIOD,    VK_NONE },
    { RETROK_SLASH,        VK_DIVIDE,    VK_NONE },
    { RETROK_SEMICOLON,    VK_SEMICOLON, VK_NONE },
    { RETROK_EQUALS,       VK_EQUALS,    VK_NONE },
    { RETROK_CAPSLOCK,     VK_CAPSLOCK,  VK_NONE },
    { RETROK_LSHIFT,       VK_SHIFT,     VK_NONE },
    { RETROK_RSHIFT,       VK_SHIFT,     VK_NONE },
    { RETROK_LCTRL,        VK_CTRL,      VK_NONE },
    { RETROK_RCTRL,        VK_CTRL,      VK_NONE },
    { RETROK_LALT,         VK_FCTN,      VK_NONE },
    { RETROK_RALT,         VK_FCTN,      VK_NONE },
    { RETROK_TAB,          VK_FCTN,      VK_7    },
    { RETROK_BACKSPACE,    VK_FCTN,      VK_S    },
    { RETROK_DELETE,       VK_FCTN,      VK_1    },
    { RETROK_UP,           VK_FCTN,      VK_E    },
    { RETROK_DOWN,         VK_FCTN,      VK_X    },
    { RETROK_LEFT,         VK_FCTN,      VK_S    },
    { RETROK_RIGHT,        VK_FCTN,      VK_D    },
    { RETROK_QUOTE,        VK_FCTN,      VK_O    },
    { RETROK_UNDERSCORE,   VK_FCTN,      VK_U    },
    { RETROK_BACKSLASH,    VK_FCTN,      VK_Z    },
    { RETROK_BACKQUOTE,    VK_FCTN,      VK_C    },
    { RETROK_LEFTBRACKET,  VK_FCTN,      VK_R    },
    { RETROK_RIGHTBRACKET, VK_FCTN,      VK_T    },
    { RETROK_LEFTBRACE,    VK_FCTN,      VK_F    },
    { RETROK_RIGHTBRACE,   VK_FCTN,      VK_G    },
    { RETROK_BAR,          VK_FCTN,      VK_A    },
    { RETROK_TILDE,        VK_FCTN,      VK_W    },
    { RETROK_QUOTEDBL,     VK_FCTN,      VK_P    },
    { RETROK_QUESTION,     VK_FCTN,      VK_I    },
};

static void bind_key ( cTMS9901 *pic, int sym, bool down, VIRTUAL_KEY_E vk1, VIRTUAL_KEY_E vk2 )
{
    if ( down )
        pic->VKeysDown ( sym, vk1, vk2 );
    else
        pic->VKeyUp ( sym );
}

static void poll_keyboard ( cTMS9901 *pic )
{
    for ( unsigned i = 0; i < sizeof ( kKeyboard ) / sizeof ( kKeyboard [0] ); i++ ) {
        const sKeyBinding *k = &kKeyboard [i];
        bool down = g_input_state ( 0, RETRO_DEVICE_KEYBOARD, 0, k->id ) != 0;
        if ( down != g_prev [k->id] ) {
            bind_key ( pic, k->id, down, k->vk1, k->vk2 );
            g_prev [k->id] = down;
        }
    }

    for ( int i = 0; i < 26; i++ ) {
        unsigned id = RETROK_a + i;
        bool down = g_input_state ( 0, RETRO_DEVICE_KEYBOARD, 0, id ) != 0;
        if ( down != g_prev [id] ) {
            bind_key ( pic, id, down, ( VIRTUAL_KEY_E ) ( VK_A + i ), VK_NONE );
            g_prev [id] = down;
        }
    }

    for ( int i = 0; i < 10; i++ ) {
        unsigned id = RETROK_0 + i;
        bool down = g_input_state ( 0, RETRO_DEVICE_KEYBOARD, 0, id ) != 0;
        if ( down != g_prev [id] ) {
            bind_key ( pic, id, down, ( VIRTUAL_KEY_E ) ( VK_0 + i ), VK_NONE );
            g_prev [id] = down;
        }
    }
}

static const sKeyBinding kJoystick [] = {
    { RETRO_DEVICE_ID_JOYPAD_UP,    VK_FCTN, VK_E },
    { RETRO_DEVICE_ID_JOYPAD_DOWN,  VK_FCTN, VK_X },
    { RETRO_DEVICE_ID_JOYPAD_LEFT,  VK_FCTN, VK_S },
    { RETRO_DEVICE_ID_JOYPAD_RIGHT, VK_FCTN, VK_D },
    { RETRO_DEVICE_ID_JOYPAD_A,     VK_ENTER, VK_NONE },
    { RETRO_DEVICE_ID_JOYPAD_B,     VK_SPACE, VK_NONE },
    { RETRO_DEVICE_ID_JOYPAD_X,     VK_FCTN, VK_S },
    { RETRO_DEVICE_ID_JOYPAD_Y,     VK_FCTN, VK_7 },
    { RETRO_DEVICE_ID_JOYPAD_START, VK_ENTER, VK_NONE },
};

static void poll_joystick ( cTMS9901 *pic )
{
    for ( unsigned i = 0; i < sizeof ( kJoystick ) / sizeof ( kJoystick [0] ); i++ ) {
        const sKeyBinding *k = &kJoystick [i];
        bool down = g_input_state ( 0, RETRO_DEVICE_JOYPAD, 0, k->id ) != 0;
        if ( down != g_prev_joy [k->id] ) {
            bind_key ( pic, JOY_SYM ( k->id ), down, k->vk1, k->vk2 );
            g_prev_joy [k->id] = down;
        }
    }
}

static void update_input ( void )
{
    if ( ! g_console )
        return;

    cTMS9901 *pic = g_console->PIC ();

    if ( g_keyboard_supported )
        poll_keyboard ( pic );
    poll_joystick ( pic );
}

// --- frame -----------------------------------------------------------------

void retro_run ( void )
{
    if ( ! g_loaded )
        return;

    if ( g_input_poll )
        g_input_poll ();
    update_input ();

    g_frame_ready = false;

    cTMS9900 *cpu = g_console->GetCPU ();
    UINT32 start = cpu->GetClocks ();
    while ( ( UINT32 ) ( cpu->GetClocks () - start ) < FRAME_CYCLES ) {
        g_console->Step ();
        if ( g_frame_ready )
            break;
    }

    // A halted CPU can leave the VDP without a retrace; force one so the
    // frontend always gets a frame back.
    if ( ! g_frame_ready )
        g_vdp->Retrace ();

    if ( g_video )
        g_video ( g_frame, VDP_W, VDP_H, VDP_W * 2 );

    if ( g_audio ) {
        int16_t samples [FRAME_SAMPLES * 2];
        g_sound->Generate ( samples, FRAME_SAMPLES );
        g_audio ( samples, FRAME_SAMPLES );
    }
}

unsigned retro_get_region ( void ) { return RETRO_REGION_NTSC; }

// --- save states -----------------------------------------------------------
//
// ti99sim's own snapshot is written to a file under $HOME by name and its size
// is not known in advance, neither of which libretro's fixed-size buffer
// supports. The machine's state is instead the sum of the pieces below: the CPU
// and the VDP serialise themselves to a stream, the memories and the GROM
// pointer come from the console, and the sound chip carries its own.

#define STATE_MAGIC   0x39394a54u
#define STATE_VERSION 1u

static bool write_state ( FILE *f )
{
    uint32_t magic = STATE_MAGIC;
    uint32_t version = STATE_VERSION;

    fwrite ( &magic, sizeof ( magic ), 1, f );
    fwrite ( &version, sizeof ( version ), 1, f );

    g_console->GetCPU ()->SaveImage ( f );
    g_vdp->SaveState ( f );
    g_console->PIC ()->SaveImage ( f );
    g_console->SaveState ( f );
    g_sound->SaveState ( f );

    fwrite ( g_console->GetCpuMemory (), 0x10000, 1, f );
    fwrite ( g_console->GetGromMemory (), 0x10000, 1, f );

    return ferror ( f ) == 0;
}

static bool read_state ( FILE *f )
{
    uint32_t magic = 0, version = 0;

    fread ( &magic, sizeof ( magic ), 1, f );
    fread ( &version, sizeof ( version ), 1, f );
    if ( magic != STATE_MAGIC || version != STATE_VERSION )
        return false;

    g_console->GetCPU ()->LoadImage ( f );
    g_vdp->LoadState ( f );
    g_console->PIC ()->LoadImage ( f );
    g_console->LoadState ( f );
    g_sound->LoadState ( f );

    fread ( g_console->GetCpuMemory (), 0x10000, 1, f );
    fread ( g_console->GetGromMemory (), 0x10000, 1, f );

    g_frame_ready = false;
    return ferror ( f ) == 0;
}

// Every field is a fixed-size image, so the size is the same whatever the
// machine is doing; measure it once with a throwaway serialise.
size_t retro_serialize_size ( void )
{
    FILE *f;
    long n;

    if ( ! g_loaded )
        return 0;
    if ( g_state_size )
        return g_state_size;

    f = tmpfile ();
    if ( ! f )
        return 0;
    write_state ( f );
    fseek ( f, 0, SEEK_END );
    n = ftell ( f );
    fclose ( f );

    if ( n > 0 )
        g_state_size = ( size_t ) n;
    return g_state_size;
}

bool retro_serialize ( void *data, size_t size )
{
    FILE *f;
    long n;
    bool ok;

    if ( ! g_loaded || size == 0 )
        return false;

    f = tmpfile ();
    if ( ! f )
        return false;
    ok = write_state ( f );
    fseek ( f, 0, SEEK_END );
    n = ftell ( f );
    rewind ( f );
    if ( ! ok || n <= 0 || ( size_t ) n > size ) {
        fclose ( f );
        return false;
    }
    ok = fread ( data, 1, ( size_t ) n, f ) == ( size_t ) n;
    fclose ( f );
    return ok;
}

bool retro_unserialize ( const void *data, size_t size )
{
    FILE *f;
    bool ok;

    if ( ! g_loaded || size == 0 )
        return false;

    f = tmpfile ();
    if ( ! f )
        return false;
    ok = fwrite ( data, 1, size, f ) == size;
    rewind ( f );
    if ( ok )
        ok = read_state ( f );
    fclose ( f );
    return ok;
}

void retro_cheat_reset ( void ) {}
void retro_cheat_set ( unsigned index, bool enabled, const char *code )
{
    ( void ) index; ( void ) enabled; ( void ) code;
}

void *retro_get_memory_data ( unsigned id ) { ( void ) id; return NULL; }
size_t retro_get_memory_size ( unsigned id ) { ( void ) id; return 0; }

bool retro_load_game_special ( unsigned type, const struct retro_game_info *info, size_t num )
{
    ( void ) type; ( void ) info; ( void ) num;
    return false;
}
