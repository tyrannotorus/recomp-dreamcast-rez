// Native interrupt plumbing: Shinobi's dispatcher, its chain registration and the
// acknowledgements, without Holly's interrupt registers.
#pragma once

namespace dream {
class System;
}

namespace rez {

// Once, before the first guest instruction.
void native_irq_start(dream::System& sys);

// For native code that acknowledges or enables Holly's events itself (the native peripherals):
// the written bits cleared from ISTNRM (0xA05F6900) or ISTERR (0xA05F6908); a mask register's new
// value; its value. Each first brings the scheduler up to the guest clock, as the register access
// would.
void irq_ack(unsigned reg, unsigned bits);
void irq_mask(unsigned reg, unsigned value);
unsigned irq_mask_value(unsigned reg);
// ISTNRM as a read of it returns it (bit 30: an external event, bit 31: an error event).
unsigned irq_status_view();

}  // namespace rez
