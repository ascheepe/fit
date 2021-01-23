CFLAGS=  -std=c89 -O2 -pipe -Wall -Wextra -Werror -pedantic
CFLAGS+= -fstack-protector-strong -fPIE -D_FORTIFY_SOURCE=2

OBJS=	fit.o
PRG=	fit

$(PRG): $(OBJS)
	$(CC) -s -o $(PRG) $(OBJS)

clean:
	rm -f $(PRG) $(OBJS)
