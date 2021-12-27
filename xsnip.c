#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>

#include <png.h>
#include <X11/Xlib.h>
#include <X11/X.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/Xmu/Atoms.h>

// escape key for exiting without screenshot
#define KQUIT 0x09 

// SAVEDIR is looked for under home directory, recompile as desired
#define SAVEDIR "/Pictures/"

// POLLRATE (ms) is the biggest determiner of CPU bottleneck
// A default of 10ms seems to work well on 60hz displays
#define POLLRATE 10

// globals so we can exit without leaks on failure anywhere
Display* display;
Window   root;
Window   overlay;
XImage*  img;

char*    fpath;
uint8_t* buffer;

int32_t exit_clean(char* err, int32_t flag) {	
	if(img) XDestroyImage(img);
	XUnmapWindow(display, overlay);

	if(fpath)  free(fpath);
	if(buffer) free(buffer);

	XCloseDisplay(display);

	if(err) fprintf(stderr, err);

	return flag;
}
int32_t create_filename(bool save, char** ts) {
	time_t cur = time(NULL);

	if(save) {
		const char* home;
		if((home = getenv("HOME")) == NULL) {
			home = getpwuid(getuid())->pw_dir;
		}

		// 29 = ctime + .png + \0
		uint32_t tsize = strlen(home) + strlen(SAVEDIR) + 29;		
		*ts = malloc(sizeof(char) * tsize);
		if(!*ts) 
			return exit_clean("Could not allocate filename\n", -1);

		strncpy(*ts, home, strlen(home));
		strncat(*ts, SAVEDIR, strlen(SAVEDIR)+1);
	} else {
		// 34 = ctime + /tmp/ + .png + \0
		*ts = malloc(sizeof(char) * 34);
		if(!*ts) 
			return exit_clean("Could not allocate filename\n", -1);
		strncpy(*ts, "/tmp/", 6);
	}

	// intentionally cut off the last 4 bytes of ctime() 
	strncat(*ts, ctime(&cur), 24);

	strncat(*ts, ".png", 6);
}

int32_t create_png(uint8_t* buffer, uint32_t width, uint32_t height, bool save, char* ts) {
	time_t cur = time(NULL);

	printf("creating %s (save=%d)\n", ts, save);

	FILE *fp = fopen(ts, "wb");
	if(!fp) 
		return exit_clean("Could not open png for writing\n", -1);

	png_bytep row_pointers[height];
	for(uint32_t i = 0; i < height; i++) {
		row_pointers[i] = (buffer + i * width * 3);
	}

	png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if(!png) 
		return exit_clean("Could not create png write struct\n", -1);

	png_infop info = png_create_info_struct(png);
	if (!info) 
		return exit_clean("Could not create png info struct\n", -1);

	if(setjmp(png_jmpbuf(png)))
		return exit_clean("Could not set png jmpbuf\n", -1);

	png_init_io(png, fp);

	png_set_IHDR(
		png, info, width, height, 8,
		PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
		PNG_COMPRESSION_TYPE_DEFAULT,
		PNG_FILTER_TYPE_DEFAULT
	);
	png_write_info(png, info);

	png_write_image(png, row_pointers);
	png_write_end(png, info);

	fclose(fp);
	png_destroy_write_struct(&png, &info);

	return 0;	
}

int main(int argc, char** argv) {
	display = XOpenDisplay(NULL);
	root = DefaultRootWindow(display);

	XWindowAttributes gwa;
	XGetWindowAttributes(display, root, &gwa);	
	
	XVisualInfo vinfo;
	XMatchVisualInfo(display, DefaultScreen(display), 32, TrueColor, &vinfo);

	XSetWindowAttributes attrs;
	attrs.override_redirect = true;
	attrs.colormap = XCreateColormap(display, root, vinfo.visual, AllocNone);
	attrs.background_pixel = 0;
	attrs.border_pixel = 0;

	overlay = XCreateWindow(
		display, root,
		0, 0, gwa.width, gwa.height, 0,
		vinfo.depth, InputOutput,
		vinfo.visual,
		CWOverrideRedirect | CWColormap | CWBackPixel | CWBorderPixel,
		&attrs);
	XMapWindow(display, overlay);

	XGCValues gcval;
	gcval.foreground = XWhitePixel(display, 0);
	gcval.function = GXxor;
	gcval.background = XBlackPixel(display, 0);
	gcval.plane_mask = gcval.background ^ gcval.foreground;
	gcval.subwindow_mode = IncludeInferiors;

	GC gc;
	gc = XCreateGC(display, overlay, GCFunction | GCForeground | GCBackground | GCSubwindowMode, &gcval);

	uint32_t wx, wy;		// junk
	Window cw, rw;			// junk

	uint32_t startx, starty, endx, endy, temp;
	bool grabbing = false;

	uint32_t mousex, mousey, mask;
	uint32_t bsx, bsy, bex, bey;
	bool save = false;

	for(;;) {
		// quit without screenshot
		/*
		XNextEvent(display, &event);
		if(event.type == KeyPress)
			if(event.xkey.keycode == KQUIT)
				return exit_clean(NULL, 0);
		*/
		XQueryPointer(display, root, &cw, &rw,
			&mousex, &mousey, &wx, &wy, &mask);

		if(mask == 256) {			// left click
			if(!grabbing) {
				save = false;
				grabbing = true;
				startx = mousex;
				starty = mousey;
			} 
		} else if(mask == 1024) {	// right click
			if(!grabbing) {
				save = true;
				grabbing = true;
				startx = mousex;
				starty = mousey;
			} 
		} else {
			if(grabbing) {
				grabbing = false;
				endx = mousex;
				endy = mousey;
				break;
			}
		}

		if(grabbing) {
			// due to the way XDrawRectange works, we always need to
			// give the upper lefthand corner first
			bsx = (mousex > startx) ? startx-1 : mousex-1;
			bsy = (mousey > starty) ? starty-1 : mousey-1;

			bex = (mousex > startx) ? mousex-startx+3 : startx-mousex+3;
			bey = (mousey > starty) ? mousey-starty+3 : starty-mousey+1;

			XClearArea(display, overlay, 0, 0, gwa.width, gwa.height, false);
			XDrawRectangle(display, overlay, gc, bsx, bsy, bex, bey);
		}
		XFlush(display);

		usleep(POLLRATE * 1000); // experiment as needed
	}

	// more corner flipping
	if(startx > endx) {
		temp = startx;
		startx = endx;
		endx = temp;
	}
	if(starty > endy) {
		temp = starty;
		starty = endy;
		endy = temp;
	}

	uint32_t width = endx+1-startx;
	uint32_t height = endy-starty;
	if(width == 0 || height == 0) return exit_clean(NULL, 0);

	img = XGetImage(display, root, 0, 0, gwa.width, gwa.height, AllPlanes, ZPixmap);
	uint32_t rmask = img->red_mask;
	uint32_t gmask = img->green_mask;
	uint32_t bmask = img->blue_mask;

	uint8_t* buffer = malloc(sizeof(uint8_t) * width * height * 3);
	if(!buffer) {
		return exit_clean("Could not allocate image buffer\n", -1);
	}


	// each pixel is encoded as an integer with colors at bit offset
	uint32_t c;
	for(uint32_t h = starty+1; h < endy+1; h++) {
		for(uint32_t w = startx+1; w < endx+2; w++) {
			uint32_t pix = XGetPixel(img, w, h);
			uint8_t r = (pix & rmask) >> 16;
			uint8_t g = (pix & gmask) >> 8;
			uint8_t b = pix & bmask;

			buffer[c++] = r;
			buffer[c++] = g;
			buffer[c++] = b;
		}
	} 

	create_filename(save, &fpath);

	int32_t png = create_png(buffer, width, height, save, fpath); 
	if(png != 0) {
		return png;
	}

	// I would have liked to do a custom clipboard implementation
	// but xlib selection handling has little documentation and
	// many features are completely broken (such as XA_CLIPBOARD).
	if(!save) {
		const char* com = "xclip -selection clipboard -target image/png -i '";
		char* command = malloc(sizeof(char) * (strlen(com) + strlen(fpath) + 2));
		if(!command)
			return exit_clean("Could not allocate command\n", -1);

		strncpy(command, com, strlen(com)+1);
		strncat(command, fpath, strlen(fpath)+1);
		strncat(command, "'", 2);

		// undesirable solution 
		system(command);

		free(command);
	}

	exit_clean(NULL, 0);

	return 0;
} 
