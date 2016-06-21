NAME= httpd
SRC = httpd

LIB_DIRS := -L$(HOME)/bin/apache/lib $(LIB_DIRS)

LIBS := -lssl -lcrypto -lapr-2 -lpcre -lz -lcrypt -lrt -ldl -lexpat $(LIBS)

include ../../Makefile.$(ACTION)

