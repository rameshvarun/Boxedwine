/*
 *  Copyright (C) 2016  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

// This code was written before the winedrv code existed.  Now this is considered dead code, but is kept for experimentation
#include "boxedwine.h"

#include <SDL.h>
#include "../../io/fsvirtualopennode.h"
#include "../../emulation//hardmmu/hard_memory.h"
#include "knativewindow.h"
#include "../../../platform/sdl/sdlcallback.h"

static U32 screenBPP=32;
static U32 updateAvailable;
static U32 paletteChanged;
static U8* screenPixels;
#ifdef BOXEDWINE_64BIT_MMU
static bool isFbActive;
static U32 screenProcessId;
bool isFbReady() {return isFbActive && KSystem::getProcess(screenProcessId);}
#endif
struct fb_fix_screeninfo {
    char id[16];			// identification string eg "TT Builtin"
    U32 smem_start;			// Start of frame buffer mem
                            // (physical address)
    U32 smem_len;           // Length of frame buffer mem
    U32 type;               // see FB_TYPE_*
    U32 type_aux;           // Interleave for interleaved Planes
    U32 visual;             // see FB_VISUAL_*
    U16 xpanstep;           // zero if no hardware panning
    U16 ypanstep;           // zero if no hardware panning
    U16 ywrapstep;          // zero if no hardware ywrap
    U32 line_length;        // length of a line in bytes
    U32 mmio_start;         // Start of Memory Mapped I/O
                            // (physical address)
    U32 mmio_len;           // Length of Memory Mapped I/O
    U32 accel;              // Indicate to driver which
                            //  specific chip/card we have
    U16 capabilities;       // see FB_CAP_*
    U16 reserved[2];        // Reserved for future compatibility
};

struct fb_cmap {
        U32 start;                    // First entry
        U32 len;                      // Number of entries
        U16 red[256];                 // Red values
        U16 green[256];
        U16 blue[256];
};

void readCMap(U32 address, struct fb_cmap* cmap) {
    U32 i = readd( address);
    U32 stop = readd(address+4)+i;
    U32 red = readd(address+8);
    U32 green = readd(address+12);
    U32 blue = readd(address+16);

    for (;i<stop;i++) {
        writew(red, cmap->red[i]); red+=2;
        writew(green, cmap->green[i]); green+=2;
        writew(blue, cmap->blue[i]); blue+=2;
    }
}

void writeFixInfo(U32 address, struct fb_fix_screeninfo* info) {
    memcopyFromNative(address, info->id, sizeof(info->id)); address+=16;
    writed(address, info->smem_start); address+=4;
    writed(address, info->smem_len); address+=4;
    writed(address, info->type); address+=4;
    writed(address, info->type_aux); address+=4;
    writed(address, info->visual); address+=4;
    writew(address, info->xpanstep); address+=2;
    writew(address, info->ypanstep); address+=2;
    writew(address, info->ywrapstep); address+=2;
    writed(address, info->line_length); address+=4;
    writed(address, info->mmio_start); address+=4;
    writed(address, info->mmio_len); address+=4;
    writed(address, info->accel); address+=4;
    writew(address, info->capabilities);
}

struct fb_bitfield {
    U32 offset;                   // beginning of bitfield
    U32 length;                   // length of bitfield
    U32 msb_right;                // != 0 : Most significant bit is
                                  // right
};

struct fb_var_screeninfo {
    U32 xres;                     // visible resolution
    U32 yres;
    U32 xres_virtual;             // virtual resolution
    U32 yres_virtual;
    U32 xoffset;                  // offset from virtual to visible
    U32 yoffset;                  // resolution

    U32 bits_per_pixel;		      // guess what
    U32 grayscale;                // 0 = color, 1 = grayscale,
                                  // >1 = FOURCC
    struct fb_bitfield red;       // bitfield in fb mem if true color,
    struct fb_bitfield green;     // else only length is significant
    struct fb_bitfield blue;
    struct fb_bitfield transp;

    U32 nonstd;                   // != 0 Non standard pixel format

    U32 activate;                 // see FB_ACTIVATE_*

    U32 height;                   // height of picture in mm
    U32 width;                    // width of picture in mm

    U32 accel_flags;              // (OBSOLETE) see fb_info.flags

    // Timing: All values in pixclocks, except pixclock (of course)
    U32 pixclock;                 // pixel clock in ps (pico seconds)
    U32 left_margin;              // time from sync to picture
    U32 right_margin;             // time from picture to sync
    U32 upper_margin;             // time from sync to picture
    U32 lower_margin;
    U32 hsync_len;                // length of horizontal sync
    U32 vsync_len;                // length of vertical sync
    U32 sync;                     // see FB_SYNC_*
    U32 vmode;                    // see FB_VMODE_*
    U32 rotate;                   // angle we rotate counter clockwise
    U32 colorspace;               // colorspace for FOURCC-based modes
    U32 reserved[4];			  // Reserved for future compatibility
};

void writeVarInfo(U32 address, struct fb_var_screeninfo* info) {
    writed(address, info->xres); address+=4;
    writed(address, info->yres); address+=4;
    writed(address, info->xres_virtual); address+=4;
    writed(address, info->yres_virtual); address+=4;
    writed(address, info->xoffset); address+=4;
    writed(address, info->yoffset); address+=4;

    writed(address, info->bits_per_pixel); address+=4;
    writed(address, info->grayscale); address+=4;

    writed(address, info->red.offset); address+=4;
    writed(address, info->red.length); address+=4;
    writed(address, info->red.msb_right); address+=4;

    writed(address, info->green.offset); address+=4;
    writed(address, info->green.length); address+=4;
    writed(address, info->green.msb_right); address+=4;

    writed(address, info->blue.offset); address+=4;
    writed(address, info->blue.length); address+=4;
    writed(address, info->blue.msb_right); address+=4;

    writed(address, info->transp.offset); address+=4;
    writed(address, info->transp.length); address+=4;
    writed(address, info->transp.msb_right); address+=4;

    writed(address, info->nonstd); address+=4;
    writed(address, info->activate); address+=4;
    writed(address, info->height); address+=4;
    writed(address, info->width); address+=4;
    writed(address, info->accel_flags); address+=4;

    writed(address, info->pixclock); address+=4;
    writed(address, info->left_margin); address+=4;
    writed(address, info->right_margin); address+=4;
    writed(address, info->upper_margin); address+=4;
    writed(address, info->lower_margin); address+=4;
    writed(address, info->hsync_len); address+=4;
    writed(address, info->vsync_len); address+=4;
    writed(address, info->sync); address+=4;
    writed(address, info->vmode); address+=4;
    writed(address, info->rotate); address+=4;
    writed(address, info->colorspace); address+=4;
    zeroMemory(address, 16);
}

U32 GET_SHIFT(U32 n) {
    U32 i;

    for (i=0;i<32;i++) {
        if (n & ((U32)1<<i))
            return i;
    }
    return 0;
}

U32 COUNT_BITS(U32 n) {
    U32 i;
    U32 result = 0;

    for (i=0;i<32;i++) {
        if (n & ((U32)1<<i)) {
            result++;
        } else if (result) {
            return result;
        }
    }
    return 0;
}

void readVarInfo(int address, struct fb_var_screeninfo* info) {
    info->xres = readd(address); address+=4;
    info->yres = readd(address); address+=4;
    info->xres_virtual = readd(address); address+=4;
    info->yres_virtual = readd(address); address+=4;
    info->xoffset = readd(address); address+=4;
    info->yoffset = readd(address); address+=4;

    info->bits_per_pixel = readd(address); address+=4;
    info->grayscale = readd(address); address+=4;

    info->red.offset = readd(address); address+=4;
    info->red.length = readd(address); address+=4;
    info->red.msb_right = readd(address); address+=4;

    info->green.offset = readd(address); address+=4;
    info->green.length = readd(address); address+=4;
    info->green.msb_right = readd(address); address+=4;

    info->blue.offset = readd(address); address+=4;
    info->blue.length = readd(address); address+=4;
    info->blue.msb_right = readd(address); address+=4;

    info->transp.offset = readd(address); address+=4;
    info->transp.length = readd(address); address+=4;
    info->transp.msb_right = readd(address); address+=4;

    info->nonstd = readd(address); address+=4;
    info->activate = readd(address); address+=4;
    info->height = readd(address); address+=4;
    info->width = readd(address); address+=4;
    info->accel_flags = readd(address); address+=4;

    info->pixclock = readd(address); address+=4;
    info->left_margin = readd(address); address+=4;
    info->right_margin = readd(address); address+=4;
    info->upper_margin = readd(address); address+=4;
    info->lower_margin = readd(address); address+=4;
    info->hsync_len = readd(address); address+=4;
    info->vsync_len = readd(address); address+=4;
    info->sync = readd(address); address+=4;
    info->vmode = readd(address); address+=4;
    info->rotate = readd(address); address+=4;
    info->colorspace = readd(address); address+=4;	
}

struct fb_var_screeninfo fb_var_screeninfo;
struct fb_fix_screeninfo fb_fix_screeninfo;
struct fb_cmap fb_cmap;
bool fbinit;
bool bOpenGL;

static SDL_Window *sdlWindow;
static SDL_GLContext sdlContext;
static SDL_Renderer *sdlRenderer;
static SDL_Texture* sdlTexture;

void destroySDL2() {
    if (sdlTexture) {
        SDL_DestroyTexture(sdlTexture);
    }
    if (sdlRenderer) {
        SDL_DestroyRenderer(sdlRenderer);
        sdlRenderer = 0;
    }
    if (sdlContext) {
        SDL_GL_DeleteContext(sdlContext);
        sdlContext = 0;
    }
    if (sdlWindow) {
        SDL_DestroyWindow(sdlWindow);
        sdlWindow = 0;
    }
}

void writeCMap(U32 address, struct fb_cmap* cmap) {
    U32 i = readd(address);
    U32 stop = readd(address+4)+i;
    U32 red = readd(address+8);
    U32 green = readd(address+12);
    U32 blue = readd(address+16);

    for (;i<stop;i++) {
        cmap->red[i] = readw(red); red+=2;
        cmap->green[i] = readw(green); green+=2;
        cmap->blue[i] = readw(blue); blue+=2;
    }
    paletteChanged = 1;	
}

void fbSetupScreenForOpenGL(int width, int height, int depth) {
    destroySDL2();
    sdlWindow = SDL_CreateWindow("", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, width, height, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    if (!sdlWindow) {
        kpanic("SDL_CreateWindow failed: %s", SDL_GetError());
    }
    sdlContext = SDL_GL_CreateContext(sdlWindow);
    if (!sdlWindow) {
        kpanic("SDL_GL_CreateContext failed: %s", SDL_GetError());
    }
    bOpenGL = 1;
}

void fbSetupScreenForMesa(int width, int height, int depth) {
    destroySDL2();
    sdlWindow = SDL_CreateWindow("", 0, 0, width, height, SDL_WINDOW_SHOWN);
    sdlRenderer = SDL_CreateRenderer(sdlWindow, -1, 0);
    sdlTexture = SDL_CreateTexture(sdlRenderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);
    bOpenGL = 1;	
}

void fbSetupScreen() {
    bOpenGL = 0;
    DISPATCH_MAIN_THREAD_BLOCK_BEGIN
    destroySDL2();
    sdlWindow = SDL_CreateWindow("", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, fb_var_screeninfo.xres, fb_var_screeninfo.yres, SDL_WINDOW_SHOWN);
    sdlRenderer = SDL_CreateRenderer(sdlWindow, -1, 0);
    sdlTexture = SDL_CreateTexture(sdlRenderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, fb_var_screeninfo.xres, fb_var_screeninfo.yres);    

    SDL_ShowCursor(0);
    DISPATCH_MAIN_THREAD_BLOCK_END

    fb_fix_screeninfo.visual = 2; // FB_VISUAL_TRUECOLOR
    fb_fix_screeninfo.type = 0; // FB_TYPE_PACKED_PIXELS
    //fb_fix_screeninfo.smem_start = ADDRESS_PROCESS_FRAME_BUFFER_ADDRESS;		

    fb_var_screeninfo.red.offset = 16;
    fb_var_screeninfo.green.offset = 8;
    fb_var_screeninfo.blue.offset = 0;
    fb_var_screeninfo.red.length = 8;			
    fb_var_screeninfo.green.length = 8;		
    fb_var_screeninfo.blue.length = 8;
    fb_fix_screeninfo.line_length = 4 * fb_var_screeninfo.xres;
#ifndef BOXEDWINE_64BIT_MMU
    screenPixels = new U8[fb_fix_screeninfo.line_length*fb_var_screeninfo.yres];
#endif
    updateAvailable = 1;
    
    fb_fix_screeninfo.smem_len = fb_fix_screeninfo.line_length*fb_var_screeninfo.yres_virtual;	
}

class FBPage : public Page {
public:
    FBPage(U32 flags) : Page(Frame_Buffer, flags) {}

    U8 readb(U32 address);
    void writeb(U32 address, U8 value);
    U16 readw(U32 address);
    void writew(U32 address, U16 value);
    U32 readd(U32 address);
    void writed(U32 address, U32 value);
    U8* getCurrentReadPtr();
    U8* getCurrentWritePtr();
    U8* getReadAddress(U32 address, U32 len);
    U8* getWriteAddress(U32 address, U32 len);
    U8* getReadWriteAddress(U32 address, U32 len);
    bool inRam() {return true;}
    void close() {delete this;}
};

Page* allocFBPage(U32 flags) {
    return new FBPage(flags);
}

U8 FBPage::readb(U32 address) {	
    if (!bOpenGL && (address-ADDRESS_PROCESS_FRAME_BUFFER_ADDRESS)<fb_fix_screeninfo.smem_len)
        return ((U8*)screenPixels)[address-ADDRESS_PROCESS_FRAME_BUFFER_ADDRESS];
    return 0;
}

void FBPage::writeb(U32 address, U8 value) {
    updateAvailable=1;
    if (!bOpenGL && (address-ADDRESS_PROCESS_FRAME_BUFFER_ADDRESS)<fb_fix_screeninfo.smem_len)
        ((U8*)screenPixels)[address-ADDRESS_PROCESS_FRAME_BUFFER_ADDRESS] = value;
}

U16 FBPage::readw(U32 address) {
    if (!bOpenGL && (address-ADDRESS_PROCESS_FRAME_BUFFER_ADDRESS)<fb_fix_screeninfo.smem_len)
        return ((U16*)screenPixels)[(address-ADDRESS_PROCESS_FRAME_BUFFER_ADDRESS)>>1];
    return 0;
}

void FBPage::writew(U32 address, U16 value) {
    updateAvailable=1;
    if (!bOpenGL && (address-ADDRESS_PROCESS_FRAME_BUFFER_ADDRESS)<fb_fix_screeninfo.smem_len)
        ((U16*)screenPixels)[(address-ADDRESS_PROCESS_FRAME_BUFFER_ADDRESS)>>1] = value;
}

U32 FBPage::readd(U32 address) {
    if (!bOpenGL && (address-ADDRESS_PROCESS_FRAME_BUFFER_ADDRESS)<fb_fix_screeninfo.smem_len)
        return ((U32*)screenPixels)[(address-ADDRESS_PROCESS_FRAME_BUFFER_ADDRESS)>>2];
    return 0;
}

void FBPage::writed(U32 address, U32 value) {
    updateAvailable=1;
    if (!bOpenGL && (address-ADDRESS_PROCESS_FRAME_BUFFER_ADDRESS)<fb_fix_screeninfo.smem_len)
        ((U32*)screenPixels)[(address-ADDRESS_PROCESS_FRAME_BUFFER_ADDRESS)>>2] = value;
}

U8* FBPage::getCurrentReadPtr() {
    return NULL;
}

U8* FBPage::getCurrentWritePtr() {
    return NULL;
}

U8* FBPage::getReadAddress(U32 address, U32 len) {    
    updateAvailable=1;
    return &((U8*)screenPixels)[address-ADDRESS_PROCESS_FRAME_BUFFER_ADDRESS];
}

U8* FBPage::getWriteAddress(U32 address, U32 len) {
    updateAvailable=1;
    return &((U8*)screenPixels)[address-ADDRESS_PROCESS_FRAME_BUFFER_ADDRESS];
}

U8* FBPage::getReadWriteAddress(U32 address, U32 len) {
    updateAvailable=1;
    return &((U8*)screenPixels)[address-ADDRESS_PROCESS_FRAME_BUFFER_ADDRESS];
}

class DevFB : public FsVirtualOpenNode {
public:
    DevFB(const BoxedPtr<FsNode>& node, U32 flags);
    virtual S64 length();
    virtual bool setLength(S64 length);
    virtual S64 getFilePointer();
    virtual S64 seek(S64 pos);
    virtual U32  map( U32 address, U32 len, S32 prot, S32 flags, U64 off);
    virtual bool canMap();
    virtual U32 ioctl(U32 request);
    virtual U32 readNative(U8* buffer, U32 len);
    virtual U32 writeNative(U8* buffer, U32 len);
    virtual void close();

    S64 pos;
};

DevFB::DevFB(const BoxedPtr<FsNode>& node, U32 flags) : FsVirtualOpenNode(node, flags), pos(0) {
    if (!fbinit) {		
        fb_fix_screeninfo.visual = 2; // FB_VISUAL_TRUECOLOR
        fb_fix_screeninfo.type = 0; // FB_TYPE_PACKED_PIXELS
        fb_fix_screeninfo.smem_start = ADDRESS_PROCESS_FRAME_BUFFER_ADDRESS;		
        fb_var_screeninfo.xres =  KNativeWindow::getNativeWindow()->screenWidth();
        fb_var_screeninfo.yres = KNativeWindow::getNativeWindow()->screenHeight();
        fb_var_screeninfo.xres_virtual = KNativeWindow::getNativeWindow()->screenWidth();
        fb_var_screeninfo.yres_virtual = KNativeWindow::getNativeWindow()->screenHeight();

        fb_var_screeninfo.bits_per_pixel = KNativeWindow::getNativeWindow()->screenBpp();
        fb_var_screeninfo.red.length = 8;			
        fb_var_screeninfo.green.length = 8;		
        fb_var_screeninfo.blue.length = 8;
        fb_var_screeninfo.transp.offset = 0;
        fb_var_screeninfo.transp.length = 0;
        fb_var_screeninfo.height = 300;
        fb_var_screeninfo.width = 400;		

        fb_fix_screeninfo.smem_len = 8*1024*1024;
        fb_fix_screeninfo.line_length = fb_var_screeninfo.width*32;
    }
}

void DevFB::close() {
#ifdef BOXEDWINE_64BIT_MMU
    KThread::currentThread()->memory->freeNativeMemory(ADDRESS_PROCESS_FRAME_BUFFER_ADDRESS >> K_PAGE_SHIFT, (16*1024*1024) >> K_PAGE_SHIFT);
    isFbActive = false;
#endif
    FsVirtualOpenNode::close();
}

S64 DevFB::length() {
    return fb_fix_screeninfo.smem_len;
}

bool DevFB::setLength(S64 len) {
    return false;
}

S64 DevFB::getFilePointer() {
    return this->pos;
}

S64 DevFB::seek(S64 pos) {
    if (pos>fb_fix_screeninfo.smem_len)
        pos = fb_fix_screeninfo.smem_len;
    this->pos = pos;
    return pos;
}

U32 DevFB::readNative(U8* buffer, U32 len) {
    if (this->pos+len>fb_fix_screeninfo.line_length)
        len = (U32)(fb_fix_screeninfo.line_length-this->pos);
    memcpy(buffer, screenPixels+this->pos, len);
    this->pos+=len;
    return len;
}

U32 DevFB::writeNative(U8* buffer, U32 len) {
    if (this->pos+len>fb_fix_screeninfo.line_length)
        len = (U32)(fb_fix_screeninfo.line_length-this->pos);
    memcpy(screenPixels+this->pos, buffer, len);
    this->pos+=len;
    return len;
}

U32 DevFB::ioctl(U32 request) {
    KThread* thread = KThread::currentThread();
    CPU* cpu=thread->cpu;

    switch(request) {
        case 0x4600: // FBIOGET_VSCREENINFO
            writeVarInfo(IOCTL_ARG1, &fb_var_screeninfo);
            break;
        case 0x4601: // FBIOPUT_VSCREENINFO
            readVarInfo(IOCTL_ARG1, &fb_var_screeninfo);
            fbSetupScreen();
            break;
        case 0x4602: // FBIOGET_FSCREENINFO
            writeFixInfo(IOCTL_ARG1, &fb_fix_screeninfo);
            break;
        case 0x4604: // FBIOGETCMAP
            readCMap(IOCTL_ARG1, &fb_cmap);
            break;
        case 0x4605: // FBIOPUTCMAP
            writeCMap(IOCTL_ARG1, &fb_cmap);
            break;
        case 0x4606: { // FBIOPAN_DISPLAY
            struct fb_var_screeninfo fb;
            readVarInfo(IOCTL_ARG1, &fb);
            break;
        }
        case 0x4611: // FBIOBLANK
            break;
        default:
            return -1;
    }
    return 0;
}

U32 DevFB::map(U32 address, U32 len, S32 prot, S32 flags, U64 off) {
    if ((flags & K_MAP_FIXED) && address!=fb_fix_screeninfo.smem_start) {
        kpanic("Mapping /dev/fb at fixed address not supported");
    }
#ifdef BOXEDWINE_64BIT_MMU
    U32 allocFlags = 0;
    if (prot & K_PROT_READ) {
        allocFlags |= PAGE_READ;
    }
    if (prot & K_PROT_WRITE) {
        allocFlags |= PAGE_WRITE;
    }
    if (flags & K_MAP_SHARED) {
        allocFlags |= PAGE_SHARED;
    }
    KThread::currentThread()->memory->allocNativeMemory(ADDRESS_PROCESS_FRAME_BUFFER_ADDRESS >> K_PAGE_SHIFT, (16 * 1024 * 1024) >> K_PAGE_SHIFT, allocFlags);
    if (isFbActive) {
        // :TODO: if and when 64-bit Boxedwine supports page sharing across processes, then we can use that
        klog("64-bit Boxedwine does not yet support /dev/fb being mapped more than 1 time");
    }
    isFbActive = true;
    screenProcessId = KThread::currentThread()->process->id;
    screenPixels = (U8*)KThread::currentThread()->memory->id;
    screenPixels += ADDRESS_PROCESS_FRAME_BUFFER_ADDRESS;
#endif
#ifdef BOXEDWINE_DEFAULT_MMU
    U32 pageStart = fb_fix_screeninfo.smem_start >> K_PAGE_SHIFT;
    U32 pageCount = (len+K_PAGE_SIZE-1)>>K_PAGE_SHIFT;
    U32 i;
    Memory* memory = KThread::currentThread()->memory;

    if (len<fb_fix_screeninfo.smem_len) {
        pageCount=fb_fix_screeninfo.smem_len >> K_PAGE_SHIFT;
    }    
    for (i=0;i<pageCount;i++) {
        if (memory->getPage(i+pageStart)->type!=Page::Type::Invalid_Page && memory->getPage(i+pageStart)->type!=Page::Type::Frame_Buffer) {
            kpanic("Something else got mapped into the framebuffer address");
        }
        memory->setPage(i+pageStart, new FBPage(flags));
    }
#endif
    return fb_fix_screeninfo.smem_start;
}

bool DevFB::canMap() {
    return true;
}

void flipFB() {
#ifdef BOXEDWINE_64BIT_MMU
    if (isFbActive && !bOpenGL && sdlTexture) {
#else
    if (updateAvailable && !bOpenGL) {
#endif

        SDL_UpdateTexture(sdlTexture, NULL, screenPixels, fb_fix_screeninfo.line_length);
        SDL_RenderClear(sdlRenderer);
        SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, NULL);
        SDL_RenderPresent(sdlRenderer);

        updateAvailable=0;
    }
}

void flipFBNoCheck() {
#ifdef BOXEDWINE_64BIT_MMU
    if (!isFbReady()) {
        return;
    }
#endif
    if (sdlTexture) {
        SDL_UpdateTexture(sdlTexture, NULL, screenPixels, fb_fix_screeninfo.line_length);
        SDL_RenderClear(sdlRenderer);
        SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, NULL);
        SDL_RenderPresent(sdlRenderer);
    }
}

void fbSetCaption(const char* title, const char* icon) {
    if (sdlWindow)
        SDL_SetWindowTitle(sdlWindow, title);
}

void fbSwapOpenGL() {
    SDL_GL_SwapWindow(sdlWindow);
}

FsOpenNode* openDevFB(const BoxedPtr<FsNode>& node, U32 flags, U32 data) {
    return new DevFB(node, flags);
}