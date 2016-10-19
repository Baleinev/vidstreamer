BIN=screenStreamerMulti
OBJS=screenStreamerMulti.o threadPollScreen.o threadVideoStream.o

CC=gcc
CFLAGS=-O2 -Wall -g

LDFLAGS=-lpthread -lx264 -lX11 -lXext -lswscale -ljpeg

all: $(BIN)

%.o: %.c %.h
	@rm -f $@ 
	$(CC) $(CFLAGS) -c $< $(LDFLAGS) -o $@ 

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -o $@ 

clean:
	for i in $(OBJS); do (if test -e "$$i"; then ( rm $$i ); fi ); done
	@rm -f $(BIN)

