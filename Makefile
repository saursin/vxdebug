BUILD_DIR?=build
LIB_DIR:=$(BUILD_DIR)/lib
OBJ_DIR:=$(BUILD_DIR)/obj
SRC_DIR:=src
LIB_SRC_DIR:=lib
$(shell mkdir -p $(BUILD_DIR) $(LIB_DIR) $(OBJ_DIR))

# Application sources (main directory)
APP_SRCS:=$(wildcard $(SRC_DIR)/*.cpp)
APP_OBJS:=$(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(APP_SRCS))
APP_DEPS:=$(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.d,$(APP_SRCS))

# Library sources (lib subdirectory)  
LIB_SRCS:=$(wildcard $(LIB_SRC_DIR)/*.cpp)
LIB_OBJS:=$(patsubst $(LIB_SRC_DIR)/%.cpp,$(OBJ_DIR)/lib%.o,$(LIB_SRCS))
LIB_DEPS:=$(patsubst $(LIB_SRC_DIR)/%.cpp,$(OBJ_DIR)/lib%.d,$(LIB_SRCS))

CFLAGS:=-g -O2 -Wall -Wextra -std=c++17 -I$(LIB_SRC_DIR) -I$(SRC_DIR) -MMD -MP
LDFLAGS:=-L$(LIB_DIR) -largparse -ltcputils -llogger -lpthread -lreadline
EXEC:=$(BUILD_DIR)/vxdebug

LIBS:= $(LIB_DIR)/libargparse.a $(LIB_DIR)/libtcputils.a $(LIB_DIR)/liblogger.a

# Include dependency files if they exist
-include $(APP_DEPS) $(LIB_DEPS)

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


# === Installation ===
PREFIX?=${HOME}/opt/bin
INSTALL_METHOD?=symlink

.PHONY: install
install: $(EXEC)
	install -d $(PREFIX)
ifeq ($(INSTALL_METHOD),symlink)
	ln -sf $(abspath $(EXEC)) $(PREFIX)/vxdebug
else ifeq ($(INSTALL_METHOD),copy)
	cp -f $(abspath $(EXEC)) $(PREFIX)/vxdebug
else
	$(error Unknown INSTALL_METHOD '$(INSTALL_METHOD)'. Use 'symlink' or 'copy'.)
endif
	@echo "Installed vxdebug to $(PREFIX)/vxdebug"

.PHONY: uninstall
uninstall:
	rm -f $(PREFIX)/vxdebug
	@echo "Uninstalled vxdebug from $(PREFIX)/vxdebug"

.PHONY: test
test: $(EXEC)
	$(EXEC) --version

.PHONY: clean
clean:
	rm -f $(OBJ_DIR)/*.o $(OBJ_DIR)/*.d $(EXEC)

.PHONY: clean-all
clean-all: clean
	rm -f $(LIB_DIR)/*.a

