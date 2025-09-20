# ZTX
Rust based Storage wiper with Alpine Linux base ISO

```text
┌─ PROTOTYPE (C + TUI) ─────────────┐    ┌─ PRODUCTION (Rust + GUI) ────┐
│                                   │    │                              │
│ Week 1-2: Core C Engine           │    │ Week 7-8: Rust Port          │
│ ├─ Raw sector I/O                 │    │ ├─ Memory-safe engine        │
│ ├─ NIST 3-pass implementation     │────┤ ├─ GTK GUI development       │
│ └─ BLAKE3 integration             │    │ └─ Enterprise features       │
│                                   │    │                              │
│ Week 3-4: System Integration      │    │ Week 9-10: Production Build  │
│ ├─ hdparm HPA/DCO unlock          │    │ ├─ Advanced GUI workflows    │
│ ├─ Device detection logic         │────┤ ├─ Batch processing          │
│ └─ Certificate generation         │    │ └─ Enterprise deployment     │
│                                   │    │                              │
│ Week 5-6: TUI + Alpine Build      │    │ Week 11-12: Final Release    │
│ ├─ ncurses interface              │    │ ├─ User acceptance testing   │
│ ├─ Progress tracking              │────┤ ├─ Documentation complete    │
│ └─ Bootable ISO creation          │    │ └─ Government certification  │
└───────────────────────────────────┘    └──────────────────────────────┘
```
```text
🚀 **DEPLOYMENT Process Flow**
USB Creation ──→ Target System ──→ ZTX Launch ──→ Operation ──→ Certificate
     │                │                 │                │               │
┌────▼─────┐    ┌─────▼──────┐    ┌─────▼──────┐   ┌─────▼──────┐   ┌────▼─────┐
│ Burn ISO │    │ Boot from  │    │  GUI Loads │   │ SCAN Phase │   │ Extract  │
│with Rufus│    │ USB drive  │    │   Device   │   │ WIPE Phase │   │ from USB │
│ (FAT32)  │    │ (F12 menu) │    │  Selection │   │ CERT Phase │   │ docs/    │
└──────────┘    └────────────┘    └────────────┘   └────────────┘   └──────────┘
```
🔄 **Technical Implementation Flow**
Hardware Detection → Hidden Sector Unlock → Secure Wiping → Verification
│                    │                    │              │
┌────▼────┐        ┌──────▼──────┐      ┌─────▼─────┐  ┌─────▼─────┐
│ /sys/block│        │hdparm -N    │      │NIST 3-pass│  │BLAKE3 hash│
│ scanning  │        │--dco-restore│      │sector wipe│  │certificate│
│ smartctl  │        │Full capacity│      │O_DIRECT   │  │PDF + JSON │
└─────────┘        └─────────────┘      └───────────┘  └───────────┘
