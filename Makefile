OUT := qdl

CXXFLAGS := -O2 -Wall -g $(shell xml2-config --cflags) -Iinclude -std=c++17
LDFLAGS := $(shell xml2-config --libs) -ludev
prefix := /usr/local

BUILD_DIR := ./build

SRCS := firehose.cpp qdl.cpp sahara.cpp patch.cpp program.cpp ufs.cpp util.cpp
OBJS = $(addprefix $(BUILD_DIR)/,$(SRCS:.cpp=.cpp.o))

$(BUILD_DIR)/%.cpp.o: %.cpp
	$(CXX) -c -o $@ $^ $(CXXFLAGS)

$(OUT): $(OBJS)
	$(CXX) -o $(BUILD_DIR)/$@ $^ $(LDFLAGS)

clean:
	rm -f $(BUILD_DIR)/$(OUT) $(OBJS)

install: $(OUT)
	install -D -m 755 $(BUILD_DIR)/$< $(DESTDIR)$(prefix)/bin/$<
