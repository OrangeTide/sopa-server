This is a webserver that is only capable of serving a single static file, and
it is not even smart enough to reload the file when it has been changed.

I wrote it specifically for the SOPA blackout. But it could be used in a pinch
for other things.

key configuration parameters are:

#define HTTP_PORT 80
#define HTTP_TIMEOUT 5
#define HTTP_BUFSIZE 128
#define HTTP_HDRMAX 512

