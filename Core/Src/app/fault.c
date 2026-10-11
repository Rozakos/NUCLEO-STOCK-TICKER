/* Cortex-M7 fault capture. The stock CubeMX handlers spin forever with no
 * output, which on this board looked like "the ticker silently froze".
 *
 * stm32f7xx_it.c is generated, so its handlers stay and only jump here from
 * their USER CODE blocks with a plain branch ("b fault_capture_N"): a branch,
 * unlike a call, leaves LR holding EXC_RETURN, which says whether the
 * exception frame is on the process stack (a task) or the main stack. */
#include "app/watchdog.h"

#include "stm32f7xx.h"

void fault_dispatch(uint32_t *sp, uint32_t kind, uint32_t on_msp)
{
  static const char *const kinds[] = { "HardFault", "MemManage", "BusFault", "UsageFault" };
  uint32_t *frame = sp;
  if (on_msp)
  {
    /* The generated handler may have pushed a register or two before
     * branching here; the real frame is the first one whose stacked xPSR
     * has the Thumb bit set. */
    for (uint32_t skip = 0; skip < 4U; ++skip)
    {
      if (sp[skip + 7U] & (1UL << 24))
      {
        frame = sp + skip;
        break;
      }
    }
  }
  fault_record_and_reset(kinds[kind & 3U], frame[6], frame[5], on_msp ? "(isr)" : "(task)");
}

#define FAULT_CAPTURE(name, index)                                \
  __attribute__((naked, used)) void name(void)                    \
  {                                                               \
    __asm volatile("tst lr, #4      \n"                           \
                   "ite eq          \n"                           \
                   "mrseq r0, msp   \n"                           \
                   "mrsne r0, psp   \n"                           \
                   "mov r1, #" #index "\n"                        \
                   "ite eq          \n"                           \
                   "moveq r2, #1    \n"                           \
                   "movne r2, #0    \n"                           \
                   "b fault_dispatch\n");                         \
  }

FAULT_CAPTURE(fault_capture_hard, 0)
FAULT_CAPTURE(fault_capture_memmanage, 1)
FAULT_CAPTURE(fault_capture_bus, 2)
FAULT_CAPTURE(fault_capture_usage, 3)
