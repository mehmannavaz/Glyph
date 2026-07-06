/* screenshot.c — take a screenshot of the X11 root window, save as PPM.
 * Usage: DISPLAY=:99 ./screenshot output.ppm
 * Build: cc -o screenshot screenshot.c -lX11
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/X.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s output.ppm\n", argv[0]);
        return 1;
    }
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "Cannot open display\n"); return 1; }
    Window root = DefaultRootWindow(d);
    int w = DisplayWidth(d, DefaultScreen(d));
    int h = DisplayHeight(d, DefaultScreen(d));

    XImage *img = XGetImage(d, root, 0, 0, w, h, AllPlanes, ZPixmap);
    if (!img) { fprintf(stderr, "Cannot get image\n"); return 1; }

    FILE *f = fopen(argv[1], "wb");
    if (!f) { fprintf(stderr, "Cannot write %s\n", argv[1]); return 1; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            unsigned long pixel = XGetPixel(img, x, y);
            unsigned char r = (pixel >> 16) & 0xFF;
            unsigned char g = (pixel >> 8) & 0xFF;
            unsigned char b = pixel & 0xFF;
            fputc(r, f); fputc(g, f); fputc(b, f);
        }
    }
    fclose(f);
    XDestroyImage(img);
    XCloseDisplay(d);
    printf("Screenshot saved: %s (%dx%d)\n", argv[1], w, h);
    return 0;
}
