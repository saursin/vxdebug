BUILD_DIR?=build
LIB_DIR:=$(BUILD_DIR)/lib
OBJ_DIR:=$(BUILD_DIR)/obj
SRC_DIR:=src
LIB_SRC_DIR:=lib
$(shell mkdir -p $(BUILD_DIR) $(LIB_DIR) $(OBJ_DIR))

# Application sources (main directory)
APP_SRCS:=$(wildcard $(SRC_DIR)/*.cpp)
APP_OBJS:=$(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(APP_SRCS))

# Library sources (lib subdirectory)  
LIB_SRCS:=$(wildcard $(LIB_SRC_DIR)/*.cpp)
LIB_OBJS:=$(patsubst $(LIB_SRC_DIR)/%.cpp,$(OBJ_DIR)/lib%.o,$(LIB_SRCS))

CFLAGS:=-g -O2 -Wall -Wextra -std=c++17 -I$(LIB_SRC_DIR) -I$(SRC_DIR)
LDFLAGS:=-L$(LIB_DIR) -largparse -ltcputils -llogger -lpthread -lreadline
EXEC:=$(BUILD_DIR)/vxdebug

LIBS:= $(LIB_DIR)/libargparse.a $(LIB_DIR)/libtcputils.a $(LIB_DIR)/liblogger.a

.PHONY: all
all: lib debugger

# === Libraries ===
.PHONY: lib
lib: $(LIBS)

# Package object files into static libraries
$(LIB_DIR)/lib%.a: $(OBJ_DIR)/lib%.o
	ar rcs $@ $<

# Compile library source files
.PRECIOUS: $(OBJ_DIR)/lib%.o
$(OBJ_DIR)/lib%.o: $(LIB_SRC_DIR)/%.cpp
	$(CXX) $(CFLAGS) -c -o $@ $<


# === Application ===
.PHONY: debugger
debugger: $(EXEC)
# Build the final executable
$(EXEC): $(APP_OBJS) $(LIBS)
	$(CXX) -o $@ $(APP_OBJS) $(LDFLAGS)

# Compile application source files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	$(CXX) $(CFLAGS) -c -o $@ $<


.PHONY: clean
clean:
	rm -f $(OBJ_DIR)/*.o $(EXEC)

.PHONY: clean-all
clean-all: clean
	rm -f $(LIB_DIR)/*.a

