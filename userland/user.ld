/* Раскладка пользовательской программы LimonOS: код с 0x400000 (ниже кучи 0x10000000 и стека). */
ENTRY(_start)
PHDRS
{
    text PT_LOAD FILEHDR PHDRS FLAGS(5);   /* R+X */
    data PT_LOAD FLAGS(6);                 /* R+W */
}
SECTIONS
{
    . = 0x400000 + SIZEOF_HEADERS;
    .text   : { *(.text .text.*) }                 :text
    .rodata : { *(.rodata .rodata.*) }             :text
    . = ALIGN(0x1000);
    .data   : { *(.data .data.*) }                 :data
    .bss    : { *(.bss .bss.*) *(COMMON) }         :data
    /DISCARD/ : { *(.eh_frame*) *(.note .note.*) *(.comment) }
}
