# tests to run as a port of the regression suite.  Leave empty to run
# all.
TESTS=

GENCERT_FLAGS=

# host to bind to during regress
REGRESS_HOST =	localhost

# set to no if don't have IPv6 working (need to listen on ::1)
HAVE_IPV6 =	yes

DISTFILES =	Makefile \
		env \
		err \
		example.mime.types \
		fcgi-test.c \
		fill-file.c \
		hello \
		invalid \
		iri_test.c \
		lib.sh \
		max-length-reply \
		puny-test.c \
		regress \
		serve-bigfile \
		slow \
		tests.sh \
		valid.ext

include ../config.mk

COBJS =		${COMPATS:.c=.o}
REG_COMPATS =	${COBJS:%=../%}

PUNY_SRCS =	puny-test.c ../puny.c ../utf8.c ../utils.c ../log.c
PUNY_OBJS =	${PUNY_SRCS:.c=.o} ${REG_COMPATS}

IRI_SRCS =	iri_test.c ../iri.c ../utf8.c ../log.c
IRI_OBJS =	${IRI_SRCS:.c=.o} ${REG_COMPATS}

.PHONY: all data clean dist

all: data puny-test iri_test fcgi-test
	env HAVE_IPV6="${HAVE_IPV6}" REGRESS_HOST="${REGRESS_HOST}" ./regress ${TESTS}

data: testdata localhost.pem testca.pem valid.crt invalid.pem

puny-test: ${PUNY_OBJS}
	${CC} ${PUNY_OBJS} -o puny-test ${LIBS} ${LDFLAGS}

iri_test: ${IRI_OBJS}
	${CC} ${IRI_OBJS} -o $@ ${LIBS} ${LDFLAGS}

fill-file: fill-file.o
	${CC} fill-file.o -o $@ ${LIBS} ${LDFLAGS}

fcgi-test: fcgi-test.o
	${CC} fcgi-test.o ../log.o ${REG_COMPATS} -o fcgi-test ${LIBS} ${LDFLAGS}

localhost.key: localhost.pem

localhost.pem:
	./../contrib/gencert ${GENCERT_FLAGS} localhost >/dev/null

testca.pem:
	openssl genrsa -out testca.key 2048
	openssl req -x509 -new -sha256		\
		-key testca.key			\
		-days 365 -nodes		\
		-out testca.pem			\
		-subj "/CN=testca"
	@echo

valid.crt: testca.pem
	openssl genrsa -out valid.key 2048
	openssl req -new -key valid.key	\
		-out valid.csr		\
		-subj "/CN=valid"
	@echo
	openssl x509 -req -in valid.csr		\
		-CA testca.pem			\
		-CAkey testca.key		\
		-CAcreateserial			\
		-out valid.crt			\
		-days 365			\
		-sha256 -extfile valid.ext

invalid.pem: localhost.pem
	cp localhost.pem invalid.pem
	cp localhost.key invalid.key

clean:
	rm -f *.o iri_test localhost.pem localhost.key
	rm -f localhost.pem localhost.key
	rm -f testca.* valid.csr valid.key valid.crt invalid.pem invalid.key
	rm -rf testdata fill-file puny-test fcgi-test
	rm -f gmid.pid fcgi.sock

testdata: fill-file
	mkdir testdata
	./fill-file testdata/bigfile
	printf "# hello world\n" > testdata/index.gmi
	cp hello slow err invalid serve-bigfile env testdata/
	cp max-length-reply testdata
	mkdir testdata/dir
	date > 'testdata/dir/current date'
	cp hello testdata/dir
	cp testdata/index.gmi testdata/dir/foo.gmi
	touch testdata/test.m3u8 testdata/foo.1

dist: ${DISTFILES}
	mkdir -p ${DESTDIR}/
	${INSTALL} -m 0644 ${DISTFILES} ${DESTDIR}/
	cd ${DESTDIR}/ && chmod +x env err hello invalid \
		max-length-reply regress slow
