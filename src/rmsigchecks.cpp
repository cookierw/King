/*
https://github.com/LinusHenze/ipwndfu_public/blob/master/rmsigchks_t8015.py
*/

#include <assert.h>
#include <vector>
#include <inttypes.h>

#include "dfu.h"
#include "usbexec.h"
#include "rmsigchecks.h"

const int HOST2DEVICE = 0x21;
const int DEVICE2HOST = 0xA1;

const int DFU_DNLOAD = 1;
const int DFU_ABORT = 4;

const uint64_t REMAP_PAGE  = 0x000000010000c000; // heapcheck
const uint64_t REMAP_PAGE2 = 0x0000000100004000; // sigcheck

const uint64_t SRAM_PAGETABLE_PAGE = 0x0000000180014000; // works

const uint64_t SRAM_REMAP_PAGE  = 0x00000001801f8000;
const uint64_t SRAM_REMAP_PAGE2 = 0x00000001801f4000;

const int PAGE_SIZE = 0x4000;

/*
    def makePTE_Page_16K(addr):
        addr >>= 14
        e = 0b11            #valid and isPage
        e |= 1      << 2    #attrIndex 1
        e |= 0b10   << 6    #AP R- in EL1, -- in EL0
        e |= 1      << 10   #AF
        e |= addr   << 14   #outputAddress
        return e
*/
uint64_t makePTE_Page_16K(uint64_t addr) {
    uint64_t a = addr >> 14;
    uint64_t e = 0x3;   // #valid and isPage
    e |= 1      << 2;   // #attrIndex 1
    e |= 0x2    << 6;   // #AP R- in EL1, -- in EL0
    e |= 1      << 10;  // #AF
    e |= a      << 14;  // #outputAddress
    return e;
}

/*
    def makePTE_Table_16K(addr):
        addr >>= 14
        e = 0b11            #valid and isTable
        e |= addr   << 14   #outputAddress
        return e
*/
uint64_t makePTE_Table_16K(uint64_t addr) {
    uint64_t a = addr >> 14;
    uint64_t e = 0x3;   // 0b11 - valid and isPage
    e |= a    << 14;    // #outputAddress
    return e;
}

/*
    Port of tihmstar/LinusHenze's python impl
*/
void remove_sigchecks() {
    printf("*** SecureROM t8015 sigcheckpath by tihmstar ***\n");

    DFU D;

    if (!D.acquire_device()) 
    {
        printf("[!] Failed to find device!\n");
        return;
    }

    if (!D.isExploited()) 
    {
        printf("[!] Device must be in PWND DFU mode. Run checkm8 and try again.\n");
        return;
    }

    string SerialNumber = D.getSerialNumber();

    if (SerialNumber.find("CPID:8015") == string::npos)
    {
        printf("[!] Only t8015 supported currently.\n");
        return;
    }

    D.release_device();

    USBEXEC U(SerialNumber);

    // make Level3 Table
    vector<uint64_t> l3table;
    for (uint64_t i = 0x0000000100000000; i < 0x0000000100100000; i += PAGE_SIZE) 
    {
        uint64_t entry = makePTE_Page_16K(i);
        if (i == REMAP_PAGE) // we are remapping heapcheck page
        {
            entry = makePTE_Page_16K(SRAM_REMAP_PAGE);
        } 
        else if (i == REMAP_PAGE2) // we are remapping sigcheck page
        {
            entry = makePTE_Page_16K(SRAM_REMAP_PAGE2);
        }
        append<uint64_t>(l3table, entry);
    }

    // we write L3 Table here
    U.write_memory_v64(SRAM_PAGETABLE_PAGE, l3table);

    // remap heapcheck page to sram
    U.cmd_memcpy(SRAM_REMAP_PAGE,REMAP_PAGE,PAGE_SIZE);

    // remap sigcheck page to sram
    U.cmd_memcpy(SRAM_REMAP_PAGE2,REMAP_PAGE2,PAGE_SIZE);

    // patch heap corruption check
    U.write_memory_uint32(0x000000010000db98-REMAP_PAGE+SRAM_REMAP_PAGE,0xC0035FD6);

    // patch codesigs
    U.write_memory_uint32(0x000000010000624c-REMAP_PAGE2+SRAM_REMAP_PAGE2,0x000080D2);

    // L2 Table point to L3
    U.write_memory_uint64(0x000000018000c400,makePTE_Table_16K(SRAM_PAGETABLE_PAGE));

    vector<uint8_t> Out;

    // memory barrier
    vector<vector<uint8_t>> memArgs;
    vector<uint8_t> arg1;
    append(arg1, 0x1000004F0);
    memArgs.push_back(arg1);
    U.execute(0, memArgs, Out);

    // Out.clear();

    // flush tlb
    vector<vector<uint8_t>> tlbArgs;
    vector<uint8_t> arg2;
    append(arg2, 0x1000004AC);
    tlbArgs.push_back(arg1);
    U.execute(0, tlbArgs, Out);

    printf("done remapping and patching page\n");
    D.acquire_device();

    D.ctrl_transfer(HOST2DEVICE, DFU_ABORT, 0, 0, 0, 0, 0);

    // Perform USB reset
    D.usb_reset();
    D.release_device();

    printf("Device is now ready to accept unsigned images\n");
}