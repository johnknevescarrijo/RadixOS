#ifndef RADIX_XTENSA_REGS_H
#define RADIX_XTENSA_REGS_H

/* Xtensa Special Register (SR) addresses */
#define SR_LBEG             0
#define SR_LEND             1
#define SR_LCOUNT           2
#define SR_SAR              3
#define SR_BR               4
#define SR_SCOMPARE1        12
#define SR_WINDOWBASE       72
#define SR_WINDOWSTART      73
#define SR_MEMCTL           97
#define SR_ATOMCTL          99
#define SR_CONFIGID0        176
#define SR_EPC1             177
#define SR_EPC2             178
#define SR_EPC3             179
#define SR_EPC4             180
#define SR_EXCSAVE1         209
#define SR_EXCSAVE2         210
#define SR_EXCCAUSE         232
#define SR_EXCVADDR         238
#define SR_VECBASE          231
#define SR_PS               230
#define SR_CCOUNT           234
#define SR_CCOMPARE0        240
#define SR_CCOMPARE1        241
#define SR_CCOMPARE2        242
#define SR_PRID             235

/* Processor Status (PS) bitfields */
#define PS_INTLEVEL_MASK    0x0000000F
#define PS_INTLEVEL_SHIFT   0
#define PS_EXCM             0x00000010
#define PS_UM               0x00000020
#define PS_RING_MASK        0x000000C0
#define PS_RING_SHIFT       6
#define PS_OWB_MASK         0x00000F00
#define PS_OWB_SHIFT        8
#define PS_CALLINC_MASK     0x00030000
#define PS_CALLINC_SHIFT    16
#define PS_WOE              0x00040000

/* Exception causes (EXCCAUSE) */
#define EXCCAUSE_ILLEGAL_INSTRUCTION    0
#define EXCCAUSE_SYSTEM_CALL            1
#define EXCCAUSE_INSTRUCTION_FETCH_ERR  2
#define EXCCAUSE_LOAD_STORE_ERR         3
#define EXCCAUSE_LEVEL1_INTERRUPT       4
#define EXCCAUSE_ALLOCA                 5
#define EXCCAUSE_INTEGER_DIVIDE_BY_ZERO 6
#define EXCCAUSE_LOAD_STORE_ALIGNMENT   9
#define EXCCAUSE_LOAD_PROHIBITED        28
#define EXCCAUSE_STORE_PROHIBITED       29

#endif /* RADIX_XTENSA_REGS_H */
