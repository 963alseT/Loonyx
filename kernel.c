#include "keyboard_map.h"

#define LINES 25
#define COLUMNS_IN_LINE 80
#define BYTES_FOR_EACH_ELEMENT 2
#define SCREENSIZE (BYTES_FOR_EACH_ELEMENT * COLUMNS_IN_LINE * LINES)

#define KEYBOARD_DATA_PORT 0x60
#define KEYBOARD_STATUS_PORT 0x64
#define IDT_SIZE 256
#define INTERRUPT_GATE 0x8e
#define KERNEL_CODE_SEGMENT_OFFSET 0x08

#define ENTER_KEY_CODE 0x1C
#define BACKSPACE_CODE 0x0E

#define VGA_COMMAND_PORT 0x3D4
#define VGA_DATA_PORT    0x3D5

#define INPUT_BUFFER_SIZE 128
char input_buffer[INPUT_BUFFER_SIZE];
int input_index = 0;

extern unsigned char keyboard_map[128];
extern void keyboard_handler(void);
/* treat port IO as unsigned bytes */
extern unsigned char read_port(unsigned short port);
extern void write_port(unsigned short port, unsigned char data);
extern void load_idt(unsigned long *idt_ptr);

/* If you don't have a strcmp implementation yet, provide one or link libc.
   Declare it so process_command compiles. */
extern int strcmp(const char *a, const char *b);

/* current cursor location (byte index into vidptr) */
unsigned int current_loc = 0;
/* video memory begins at address 0xb8000 */
char *vidptr = (char*)0xb8000;

struct IDT_entry {
    unsigned short int offset_lowerbits;
    unsigned short int selector;
    unsigned char zero;
    unsigned char type_attr;
    unsigned short int offset_higherbits;
};

struct IDT_entry IDT[IDT_SIZE];

/* simple scroll function */
static void scroll_if_needed(void)
{
    if (current_loc < SCREENSIZE) return;

    unsigned int line_bytes = BYTES_FOR_EACH_ELEMENT * COLUMNS_IN_LINE;
    unsigned int i;

    /* shift framebuffer up (byte-wise) */
    for (i = 0; i < SCREENSIZE - line_bytes; ++i)
        vidptr[i] = vidptr[i + line_bytes];

    /* clear last line */
    for (i = SCREENSIZE - line_bytes; i < SCREENSIZE; i += 2) {
        vidptr[i] = ' ';
        vidptr[i + 1] = 0x07;
    }

    current_loc -= line_bytes;
}

void move_cursor() {
    int pos = current_loc / 2;

    // send high byte
    write_port(VGA_COMMAND_PORT, 14);
    write_port(VGA_DATA_PORT, (pos >> 8) & 0xFF);

    // send low byte
    write_port(VGA_COMMAND_PORT, 15);
    write_port(VGA_DATA_PORT, pos & 0xFF);
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

int strlen(const char *s) {
    int len = 0;
    while (s[len] != '\0') {
        len++;
    }
    return len;
}

int strncmp(const char *s1, const char *s2, int n) {
    int i = 0;
    while (i < n && s1[i] != '\0' && s2[i] != '\0') {
        if (s1[i] != s2[i]) {
            return (unsigned char)s1[i] - (unsigned char)s2[i];
        }
        i++;
    }
    if (i == n) {
        return 0;
    }
    return (unsigned char)s1[i] - (unsigned char)s2[i];
}

void idt_init(void)
{
    unsigned long keyboard_address;
    unsigned long idt_address;
    unsigned long idt_ptr[2];

    /* populate IDT entry of keyboard's interrupt */
    keyboard_address = (unsigned long)keyboard_handler;
    IDT[0x21].offset_lowerbits = keyboard_address & 0xffff;
    IDT[0x21].selector = KERNEL_CODE_SEGMENT_OFFSET;
    IDT[0x21].zero = 0;
    IDT[0x21].type_attr = INTERRUPT_GATE;
    IDT[0x21].offset_higherbits = (keyboard_address & 0xffff0000) >> 16;

    /* PIC remapping sequence (ICW1..4) */
    /* ICW1 - begin initialization */
    write_port(0x20 , 0x11);
    write_port(0xA0 , 0x11);

    /* ICW2 - remap offset address of IDT: master->0x20, slave->0x28 */
    write_port(0x21 , 0x20);
    write_port(0xA1 , 0x28);

    /* ICW3 - setup cascading: master has slave on IRQ2 (bit 2 -> 0x04), slave id = 2 */
    write_port(0x21 , 0x04);
    write_port(0xA1 , 0x02);

    /* ICW4 - environment info (8086 mode) */
    write_port(0x21 , 0x01);
    write_port(0xA1 , 0x01);
    /* Initialization finished */

    /* mask interrupts (disable) */
    write_port(0x21 , 0xff);
    write_port(0xA1 , 0xff);

    /* fill the IDT descriptor correctly: limit = size-1, base = address */
    idt_address = (unsigned long)IDT;
    unsigned long idt_limit = (sizeof(struct IDT_entry) * IDT_SIZE) - 1;

    /* idt_ptr[0] = limit (low16) | (base & 0xFFFF) << 16
       idt_ptr[1] = base >> 16  (fits in low 16 bits) */
    idt_ptr[0] = (idt_limit & 0xFFFF) | ((idt_address & 0xFFFF) << 16);
    idt_ptr[1] = (idt_address >> 16) & 0xFFFF;

    load_idt(idt_ptr);
}

void kb_init(void)
{
    /* 0xFD is 11111101 - enables only IRQ1 (keyboard) on master PIC */
    write_port(0x21 , 0xFD);
    /* slave stays masked (0xFF) until you want other IRQs */
}

void kprint(const char *str)
{
    unsigned int i = 0;
    while (str[i] != '\0') {
        vidptr[current_loc++] = str[i++];
        vidptr[current_loc++] = 0x07;
        scroll_if_needed();
    }
    move_cursor();
}

void kprint_newline(void)
{
    unsigned int line_size = BYTES_FOR_EACH_ELEMENT * COLUMNS_IN_LINE;
    current_loc = current_loc + (line_size - current_loc % (line_size));
    move_cursor();
    scroll_if_needed();
}

void clear_screen(void)
{
    unsigned int i = 0;
    while (i < SCREENSIZE) {
        vidptr[i++] = ' ';
        vidptr[i++] = 0x07;
    }
    current_loc = 0;
}

void cowsay(const char *msg) {
    int len = strlen(msg);

    kprint("+");
    for (int i = 0; i < len + 2; i++) {
        kprint("-");
    }
    kprint("+");
    kprint_newline();

    kprint("| ");
    kprint(msg);
    kprint(" |");
    kprint_newline();

    kprint("+");
    for (int i = 0; i < len + 2; i++) {
        kprint("-");
    }
    kprint("+");
    kprint_newline();

    kprint("   \\   ^__^");
    kprint_newline();
    kprint("    \\  (oo)\\_______");
    kprint_newline();
    kprint("       (__)\\       )\\/\\");
    kprint_newline();
    kprint("           ||----w |");
    kprint_newline();
    kprint("           ||     ||");
    kprint_newline();
}

void process_command(const char *cmd) {
    if (strcmp(cmd, "hello") == 0) {
        kprint_newline();
        kprint("Hi there!");
        kprint_newline();
    } else if (strcmp(cmd, "clear") == 0) {
        clear_screen();
        current_loc = 0;
    } else if (strcmp(cmd, "about") == 0) {
        kprint_newline();
        kprint("My first kernel :)");
        kprint_newline();
    } else if (strcmp(cmd, "cheese") == 0) {
        for (int i = 0; i < 100; i++) {
            kprint_newline();
            kprint("Cheese!");
        }
    } else if(strncmp(cmd, "cowsay ", 7) == 0) {
        kprint_newline();
        const char *msg = cmd + 7;
        cowsay(msg);
    } else {
        kprint_newline();
        kprint("Unknown command");
        kprint_newline();
    }
}

/* keyboard handler main: minimal robust behaviour
   - ignore break codes (key releases)
   - put chars into input_buffer, backspace works, Enter processes command
   - send EOI after handling
*/
void keyboard_handler_main(void)
{
    unsigned char status;
    unsigned char keycode;

    status = read_port(KEYBOARD_STATUS_PORT);
    /* Lowest bit of status will be set if output buffer is full (data available) */
    if (status & 0x01) {
        keycode = read_port(KEYBOARD_DATA_PORT);

        /* ignore break codes (key up) */
        if (keycode & 0x80) {
            /* consume and ignore */
            write_port(0x20, 0x20); /* EOI */
            return;
        }

        /* Enter pressed: terminate buffer and process */
        if (keycode == ENTER_KEY_CODE) {
            input_buffer[input_index] = '\0';
            process_command(input_buffer);
            input_index = 0;
            kprint_newline();
            write_port(0x20, 0x20); /* EOI */
            return;
        }

        /* Backspace: remove previous character */
        if (keycode == BACKSPACE_CODE) {
            if (input_index > 0) {
                input_index--;
                if (current_loc >= 2) {
                    current_loc -= 2;
                    vidptr[current_loc] = ' ';
                    vidptr[current_loc + 1] = 0x07;
                    move_cursor();
                } else {
                    current_loc = 0;
                }
            }
            write_port(0x20, 0x20); /* EOI */
            return;
        }

        /* Normal printable char: check mapping and buffer space */
        if (keycode < 128) {
            char c = keyboard_map[keycode];
            if (c && input_index < INPUT_BUFFER_SIZE - 1) {
                input_buffer[input_index++] = c;
                vidptr[current_loc++] = c;
                vidptr[current_loc++] = 0x07;
                move_cursor();
                scroll_if_needed();
            }
        }

        /* send EOI for master PIC (keyboard IRQ1) */
        write_port(0x20, 0x20);
    }
}

void kmain(void)
{
    const char *str = "Welcome to Loonyx, the world's greatest operating system";
    clear_screen();
    kprint(str);
    kprint_newline();
    kprint_newline();

    idt_init();
    kb_init();

    while(1);
}
