CFLAGS = -Wall -W -Wshadow -g
#CFLAGS += -O0
CFLAGS += -O2 -DNDEBUG
sopa_server : sopa_server.c
clean :
	$(RM) sopa_server
