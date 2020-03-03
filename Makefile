OUT := qdl

CXXFLAGS := -O2 -Wall -g `xml2-config --cflags`
LDFLAGS := `xml2-config --libs` -ludev
prefix := /usr/local

SRCS := firehose.cpp qdl.cpp sahara.cpp patch.cpp program.cpp ufs.cpp util.cpp
OBJS := $(SRCS:.cpp=.o)

$(OUT): $(OBJS)
	$(CXX) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(OUT) $(OBJS)

install: $(OUT)
	install -D -m 755 $< $(DESTDIR)$(prefix)/bin/$<
