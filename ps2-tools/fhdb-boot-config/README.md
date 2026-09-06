# FHDB HDD Boot Configuration

A deliberately small, standalone PS2 ELF used by PS2 HDD Manager.

It reads the console OSD configuration at startup and **does not write anything automatically**.
The UI provides explicit Enable, Disable and Re-read/Verify actions. Enable uses the same canonical
low-bit value (`0x02`) used by Free McBoot/Free HDBoot installers. Disable sets the HDD-MBR skip bit
while preserving the rest of the OSD configuration byte. Every write is followed by an immediate
read-back verification.

Build with an initialized PS2DEV/PS2SDK environment:

```bash
make clean all
```

The result is `FHDB-Boot-Config.ELF`.
