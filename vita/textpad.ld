/* (vita-elf-create puts ~3.5 KB of module data after the end of the code segment, which must not reach the data
 * segment: that starts at the next 64 KB boundary. When the code ends within 4 KB of one, this pads it past the
 * boundary, so the data goes to the following one. Added to the default linker script: see CMakeLists.txt. The
 * section takes its read-only flags from nr_textpad in io.c) */
SECTIONS
{
  .vitapad :
  {
    KEEP(*(.nrtextpad))
    . = ((ABSOLUTE(.) & 0xFFFF) > 0xEFFF) ? . + 0x1100 : .;
  }
}
INSERT AFTER .eh_frame;
