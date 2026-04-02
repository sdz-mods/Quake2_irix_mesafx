# Quake2 IRIX MesaFX port

Quake2 with MesaFX backend port for IRIX.

## Building

### Prerequisites

Install the complete IRIX development environment:

- MIPSPro 7.4.4 compiler (install 7.4, then patch to 7.4.4m)
- Development Foundation 1.3
- Development Libraries February 2002 (latest version)

Build and install the IRIX 3dfx kernel driver:
- https://github.com/sdz-mods/tdfx_irix

Build and install the IRIX glide port:
- https://github.com/sdz-mods/glide_irix

Build and install the IRIX MesaFX port:
- https://github.com/sdz-mods/MesaFX_irix


### Build and Install

```csh
#clone or copy this repo onto the target system, e.g. /usr/3dfx_irix/Quake2_irix_minigl
cd /usr/3dfx_irix/Quake2_irix_minigl
smake -f irix/Makefile.irixfx MESA_ROOT=/usr/sdd/3dfx_irix/MesaFX_irix GLIDE_ROOT=/usr/sdd/3dfx_irix/glide_irix

```


## Tested with

- IP32, RM7000C CPU, Irix 6.5.30, Voodoo1, glide2x library, MesaFX library


## License

Source code is licensed under GPL.
