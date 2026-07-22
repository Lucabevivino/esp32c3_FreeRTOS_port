
# Toolchain
TOOLCHAIN_BIN = /Users/lucabevivino/xpack_rv_gcc_15.2/bin/
CC      = $(TOOLCHAIN_BIN)riscv-none-elf-gcc
OBJCOPY = $(TOOLCHAIN_BIN)riscv-none-elf-objcopy
SIZE    = $(TOOLCHAIN_BIN)riscv-none-elf-size
GDB     = $(TOOLCHAIN_BIN)riscv-none-elf-gdb

# Esptool
ESPTOOL = esptool.py
PORT    = /dev/cu.usbmodem*
BAUD    = 921600


# Paths
FREERTOS_DIR     = ./FreeRTOS-Kernel
FREERTOS_PORT    = ./bare_metal_port
FREERTOS_MEMMANG = $(FREERTOS_DIR)/portable/MemMang
BUILD			 = ./build
DRIVERS      	 = ./drivers
BSP              = ./bsp
COMMON			 = ./common

# Target
TARGET 			= freertos_esp32c3
TARGET_ELF      = $(BUILD)/$(TARGET).elf
TARGET_BIN      = $(BUILD)/$(TARGET).bin
TARGET_MAP		= $(BUILD)/$(TARGET).map
RAM_BIN     	= $(BUILD)/ram_image.bin

# Headers
INC_DIRS += -I. -I$(FREERTOS_DIR)/include -I$(FREERTOS_PORT) -I$(DRIVERS) -I$(BSP) -I$(COMMON)

# Sources
SRCS += $(BSP)/boot.c 
SRCS += $(FREERTOS_DIR)/tasks.c $(FREERTOS_DIR)/list.c $(FREERTOS_DIR)/queue.c \
        $(FREERTOS_DIR)/timers.c $(FREERTOS_DIR)/event_groups.c \
        $(FREERTOS_DIR)/stream_buffer.c $(FREERTOS_DIR)/croutine.c
SRCS += $(DRIVERS)/uart/uart_hal.c
SRCS += $(FREERTOS_PORT)/port.c $(FREERTOS_PORT)/port_systimer.c
SRCS += $(FREERTOS_MEMMANG)/heap_4.c
SRCS += main.c

ASMS += $(FREERTOS_PORT)/port_asm.S

# Compile options
CFLAGS  = -march=rv32im_zicsr -mabi=ilp32 -g3 -O2
CFLAGS += -ffunction-sections -fdata-sections
CFLAGS += -msmall-data-limit=8 -fno-common
CFLAGS += -Wall $(INC_DIRS)

# Linker options
LDFLAGS = -T $(BSP)/linker_mdk.ld \
          -Wl,-Map=$(TARGET_MAP) \
          -Wl,--gc-sections \
          -nostartfiles \
          --specs=nosys.specs \
          -lc -lgcc

# Object files
OBJS = $(ASMS:%.S=$(BUILD)/%.o) $(SRCS:%.c=$(BUILD)/%.o)

.PHONY: all clean flash monitor debug

all: $(TARGET_BIN)

$(TARGET_ELF): $(OBJS)
	@echo "Linking $@"
	@$(CC) $(OBJS) $(LDFLAGS) -o $@
	@$(SIZE) $@

$(TARGET_BIN): $(TARGET_ELF)
	@echo "Generating Bin..."
	@$(ESPTOOL) --chip esp32c3 elf2image --flash_mode dio --flash_freq 40m --flash_size 4MB -o $@ $<

run: $(TARGET_ELF)
	@echo "Generating image for RAM..."
	$(ESPTOOL) --chip esp32c3 elf2image --ram-only-header -o $(RAM_BIN) $(TARGET_ELF)
	@echo "Caricamento in RAM..."
	$(ESPTOOL) --chip esp32c3 --port $(PORT) --baud $(BAUD) --no-stub load-ram $(RAM_BIN)

debug: $(TARGET_ELF)
	@echo "Starting GDB..."
	$(GDB) -ex "target extended-remote :3333" $<

monitor:
	screen $(PORT) 115200

clean:
	@rm -rf $(BUILD) 

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@
