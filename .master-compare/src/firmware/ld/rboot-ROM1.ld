MEMORY
{
  dport0_0_seg :                        org = 0x3FF00000, len = 0x10
  dram0_0_seg :                         org = 0x3FFE8000, len = 0x14000
  irom0_0_seg :                         org = 0x40202010, len = 0xFDFF0
}

PROVIDE ( _FS_start  = 0x40400000 );
PROVIDE ( _FS_end    = 0x404FB000 );
PROVIDE ( _FS_page   = 0x100 );
PROVIDE ( _FS_block  = 0x2000 );
PROVIDE ( _EEPROM_start = 0x405FB000 );

PROVIDE ( _SPIFFS_start = 0x40400000 );
PROVIDE ( _SPIFFS_end   = 0x404FB000 );
PROVIDE ( _SPIFFS_page  = 0x100 );
PROVIDE ( _SPIFFS_block = 0x2000 );

INCLUDE "local.eagle.app.v6.common.ld"
