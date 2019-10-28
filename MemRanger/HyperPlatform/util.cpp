// Copyright (c) 2015-2019, Satoshi Tanda. All rights reserved.
// Use of this source code is governed by a MIT-style license that can be
// found in the LICENSE file.

/// @file
/// Implements primitive utility functions.
//#include <ntddk.h>
//#include <ntimage.h>
#include "util.h"
#include <intrin.h>
#include "asm.h"
#include "common.h"
#include "log.h"

extern "C" {
////////////////////////////////////////////////////////////////////////////////
//
// macro utilities
//

////////////////////////////////////////////////////////////////////////////////
//
// constants and macros
//

// Use RtlPcToFileHeader if available. Using the API causes a broken font bug
// on the 64 bit Windows 10 and should be avoided. This flag exist for only
// further investigation.
static const auto kUtilpUseRtlPcToFileHeader = false;

////////////////////////////////////////////////////////////////////////////////
//
// types
//

NTKERNELAPI PVOID NTAPI RtlPcToFileHeader(_In_ PVOID PcValue,
                                          _Out_ PVOID *BaseOfImage);

using RtlPcToFileHeaderType = decltype(RtlPcToFileHeader);

_Must_inspect_result_ _IRQL_requires_max_(DISPATCH_LEVEL) NTKERNELAPI
    _When_(return != NULL, _Post_writable_byte_size_(NumberOfBytes)) PVOID
    MmAllocateContiguousNodeMemory(
        _In_ SIZE_T NumberOfBytes,
        _In_ PHYSICAL_ADDRESS LowestAcceptableAddress,
        _In_ PHYSICAL_ADDRESS HighestAcceptableAddress,
        _In_opt_ PHYSICAL_ADDRESS BoundaryAddressMultiple, _In_ ULONG Protect,
        _In_ NODE_REQUIREMENT PreferredNode);

using MmAllocateContiguousNodeMemoryType =
    decltype(MmAllocateContiguousNodeMemory);

//
// DRIVER_OBJECT.DriverSection type
// see Reverse Engineering site:
// https://revers.engineering/author/daax/
//
// struct KLdrDataTableEntry {
//     LIST_ENTRY in_load_order_links;
//     PVOID exception_table;
//     UINT32 exception_table_size;
//     // ULONG padding on IA64
//     PVOID gp_value;
//     PVOID /*PNON_PAGED_DEBUG_INFO*/ non_paged_debug_info;
//     PVOID dll_base;
//     PVOID entry_point;
//     UINT32 size_of_image;
//     UNICODE_STRING full_dll_name;
//     UNICODE_STRING base_dll_name;
//     UINT32 flags;
//     UINT16 load_count;
// 
//     union {
//       UINT16 signature_level : 4;
//       UINT16 signature_type : 3;
//       UINT16 unused : 9;
//       UINT16 entire_field;
//     } u;
// 
//     PVOID section_pointer;
//     UINT32 checksum;
//     UINT32 coverage_section_size;
//     PVOID coverage_section;
//     PVOID loaded_imports;
//     PVOID spare;
//     UINT32 size_of_image_not_rouned;
//     UINT32 time_date_stamp;
//  };

struct LdrDataTableEntry {
  LIST_ENTRY in_load_order_links;
  LIST_ENTRY in_memory_order_links;
  LIST_ENTRY in_initialization_order_links;
  void *dll_base;
  void *entry_point;
  ULONG size_of_image;
  UNICODE_STRING full_dll_name;
  // ...
};

////////////////////////////////////////////////////////////////////////////////
//
// prototypes
//

_IRQL_requires_max_(PASSIVE_LEVEL) static NTSTATUS
    UtilpInitializePageTableVariables();

_IRQL_requires_max_(PASSIVE_LEVEL) static NTSTATUS
    UtilpInitializeRtlPcToFileHeader(_In_ PDRIVER_OBJECT driver_object);


/*  */
_Success_(return != nullptr) static PVOID NTAPI
UtilpUnsafeGetNTFileHeaderAdvanced(PVOID *base_of_image, ULONG &size_of_image);

/*  */
_Success_(return != nullptr) static PVOID NTAPI
UtilpUnsafePcToFileHeaderAdvanced(__in PVOID pc_value, PVOID *base_of_image, ULONG &size_of_image);

/*  */
_Success_(return != nullptr) static PVOID NTAPI
UtilpUnsafeGetAnyModuleFileHeader(UNICODE_STRING &str, PVOID *base_of_image, ULONG &size_of_image);


_Success_(return != nullptr) static PVOID NTAPI
    UtilpUnsafePcToFileHeader(_In_ PVOID pc_value, _Out_ PVOID *base_of_image);

_IRQL_requires_max_(PASSIVE_LEVEL) static NTSTATUS
    UtilpInitializePhysicalMemoryRanges();

_IRQL_requires_max_(PASSIVE_LEVEL) static PhysicalMemoryDescriptor
    *UtilpBuildPhysicalMemoryRanges();

static bool UtilpIsCanonicalFormAddress(_In_ void *address);

static HardwarePte *UtilpAddressToPxe(_In_ const void *address);

static HardwarePte *UtilpAddressToPpe(_In_ const void *address);

static HardwarePte *UtilpAddressToPde(_In_ const void *address);

static HardwarePte *UtilpAddressToPte(_In_ const void *address);

#if defined(ALLOC_PRAGMA)
#pragma alloc_text(INIT, UtilInitialization)
#pragma alloc_text(PAGE, UtilTermination)
#pragma alloc_text(INIT, UtilpInitializePageTableVariables)
#pragma alloc_text(INIT, UtilpInitializeRtlPcToFileHeader)
#pragma alloc_text(PAGE, UtilpGetNTHeaderInfo)
#pragma alloc_text(PAGE, UtilpGetModuleBasenSize)
#pragma alloc_text(PAGE, UtilpGetAnyModuleHeaderInfo)
#pragma alloc_text(PAGE, UtilpUnsafeGetAnyModuleFileHeader)
#pragma alloc_text(PAGE, UtilpUnsafeGetNTFileHeaderAdvanced)
#pragma alloc_text(PAGE, UtilpUnsafePcToFileHeaderAdvanced)
#pragma alloc_text(PAGE, UtilpUnsafePcToFileHeader)
#pragma alloc_text(PAGE, UtilPcToFileHeader)


#pragma alloc_text(INIT, UtilpInitializePhysicalMemoryRanges)
#pragma alloc_text(PAGE, UtilpBuildPhysicalMemoryRanges)
#pragma alloc_text(PAGE, UtilGetPhysicalMemoryRanges)

#pragma alloc_text(PAGE, UtilForEachProcessor)
#pragma alloc_text(PAGE, UtilForEachProcessorDpc)
#pragma alloc_text(PAGE, UtilSleep)
#pragma alloc_text(PAGE, UtilMemMem)
#pragma alloc_text(PAGE, UtilGetSystemProcAddress)


#endif

////////////////////////////////////////////////////////////////////////////////
//
// variables
//

static RtlPcToFileHeaderType *g_utilp_RtlPcToFileHeader;

static LIST_ENTRY *g_utilp_PsLoadedModuleList;

static PhysicalMemoryDescriptor *g_utilp_physical_memory_ranges;

static MmAllocateContiguousNodeMemoryType
    *g_utilp_MmAllocateContiguousNodeMemory;

static ULONG_PTR g_utilp_pxe_base = 0;
static ULONG_PTR g_utilp_ppe_base = 0;
static ULONG_PTR g_utilp_pde_base = 0;
static ULONG_PTR g_utilp_pte_base = 0;

static ULONG_PTR g_utilp_pxi_shift = 0;
static ULONG_PTR g_utilp_ppi_shift = 0;
static ULONG_PTR g_utilp_pdi_shift = 0;
static ULONG_PTR g_utilp_pti_shift = 0;

static ULONG_PTR g_utilp_pxi_mask = 0;
static ULONG_PTR g_utilp_ppi_mask = 0;
static ULONG_PTR g_utilp_pdi_mask = 0;
static ULONG_PTR g_utilp_pti_mask = 0;

////////////////////////////////////////////////////////////////////////////////
//
// implementations
//

// Initializes utility functions
_Use_decl_annotations_ NTSTATUS
UtilInitialization(PDRIVER_OBJECT driver_object) {
  PAGED_CODE();

  auto status = UtilpInitializePageTableVariables();
  HYPERPLATFORM_LOG_DEBUG(
      "PXE at %016Ix, PPE at %016Ix, PDE at %016Ix, PTE at %016Ix",
      g_utilp_pxe_base, g_utilp_ppe_base, g_utilp_pde_base, g_utilp_pte_base);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  status = UtilpInitializeRtlPcToFileHeader(driver_object);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  status = UtilpInitializePhysicalMemoryRanges();
  if (!NT_SUCCESS(status)) {
    return status;
  }

  g_utilp_MmAllocateContiguousNodeMemory =
      static_cast<MmAllocateContiguousNodeMemoryType *>(
          UtilGetSystemProcAddress(L"MmAllocateContiguousNodeMemory"));
  return status;
}

// Terminates utility functions
_Use_decl_annotations_ void UtilTermination() {
  PAGED_CODE();

  if (g_utilp_physical_memory_ranges) {
    ExFreePoolWithTag(g_utilp_physical_memory_ranges,
                      kHyperPlatformCommonPoolTag);
  }
}

// Initializes g_utilp_p*e_base, g_utilp_p*i_shift and g_utilp_p*i_mask.
_Use_decl_annotations_ static NTSTATUS UtilpInitializePageTableVariables() {
  PAGED_CODE();

#include "util_page_constants.h"  // Include platform dependent constants

  // Check OS version to know if page table base addresses need to be relocated
  RTL_OSVERSIONINFOW os_version;
  os_version.dwOSVersionInfoSize = sizeof(os_version);
  auto status = RtlGetVersion(&os_version);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  // Win 10 build 14316 is the first version implements randomized page tables
  // Use fixed values if a systems is either: x86, older than Windows 7, or
  // older than build 14316.
  if (!IsX64() || os_version.dwMajorVersion < 10 ||
      os_version.dwBuildNumber < 14316) {
    if (IsX64()) {
      g_utilp_pxe_base = kUtilpPxeBase;
      g_utilp_ppe_base = kUtilpPpeBase;
      g_utilp_pxi_shift = kUtilpPxiShift;
      g_utilp_ppi_shift = kUtilpPpiShift;
      g_utilp_pxi_mask = kUtilpPxiMask;
      g_utilp_ppi_mask = kUtilpPpiMask;
    }
    if (UtilIsX86Pae()) {
      g_utilp_pde_base = kUtilpPdeBasePae;
      g_utilp_pte_base = kUtilpPteBasePae;
      g_utilp_pdi_shift = kUtilpPdiShiftPae;
      g_utilp_pti_shift = kUtilpPtiShiftPae;
      g_utilp_pdi_mask = kUtilpPdiMaskPae;
      g_utilp_pti_mask = kUtilpPtiMaskPae;
    } else {
      g_utilp_pde_base = kUtilpPdeBase;
      g_utilp_pte_base = kUtilpPteBase;
      g_utilp_pdi_shift = kUtilpPdiShift;
      g_utilp_pti_shift = kUtilpPtiShift;
      g_utilp_pdi_mask = kUtilpPdiMask;
      g_utilp_pti_mask = kUtilpPtiMask;
    }
    return status;
  }

  // Get PTE_BASE from MmGetVirtualForPhysical
  const auto p_MmGetVirtualForPhysical =
      UtilGetSystemProcAddress(L"MmGetVirtualForPhysical");
  if (!p_MmGetVirtualForPhysical) {
    return STATUS_PROCEDURE_NOT_FOUND;
  }

  static const UCHAR kPatternWin10x64[] = {
      0x48, 0x8b, 0x04, 0xd0,  // mov     rax, [rax+rdx*8]
      0x48, 0xc1, 0xe0, 0x19,  // shl     rax, 19h
      0x48, 0xba,              // mov     rdx, ????????`????????  ; PTE_BASE
  };
  auto found = reinterpret_cast<ULONG_PTR>(
      UtilMemMem(p_MmGetVirtualForPhysical, 0x30, kPatternWin10x64,
                 sizeof(kPatternWin10x64)));
  if (!found) {
    return STATUS_PROCEDURE_NOT_FOUND;
  }

  found += sizeof(kPatternWin10x64);
  HYPERPLATFORM_LOG_DEBUG("Found a hard coded PTE_BASE at %016Ix", found);

  const auto pte_base = *reinterpret_cast<ULONG_PTR *>(found);
  const auto index = (pte_base >> kUtilpPxiShift) & kUtilpPxiMask;
  const auto pde_base = pte_base | (index << kUtilpPpiShift);
  const auto ppe_base = pde_base | (index << kUtilpPdiShift);
  const auto pxe_base = ppe_base | (index << kUtilpPtiShift);

  g_utilp_pxe_base = static_cast<ULONG_PTR>(pxe_base);
  g_utilp_ppe_base = static_cast<ULONG_PTR>(ppe_base);
  g_utilp_pde_base = static_cast<ULONG_PTR>(pde_base);
  g_utilp_pte_base = static_cast<ULONG_PTR>(pte_base);

  g_utilp_pxi_shift = kUtilpPxiShift;
  g_utilp_ppi_shift = kUtilpPpiShift;
  g_utilp_pdi_shift = kUtilpPdiShift;
  g_utilp_pti_shift = kUtilpPtiShift;

  g_utilp_pxi_mask = kUtilpPxiMask;
  g_utilp_ppi_mask = kUtilpPpiMask;
  g_utilp_pdi_mask = kUtilpPdiMask;
  g_utilp_pti_mask = kUtilpPtiMask;
  return status;
}

// Locates RtlPcToFileHeader
_Use_decl_annotations_ static NTSTATUS UtilpInitializeRtlPcToFileHeader(
    PDRIVER_OBJECT driver_object) {
  PAGED_CODE();

  if (kUtilpUseRtlPcToFileHeader) {
    const auto p_RtlPcToFileHeader =
        UtilGetSystemProcAddress(L"RtlPcToFileHeader");
    if (p_RtlPcToFileHeader) {
      g_utilp_RtlPcToFileHeader =
          static_cast<RtlPcToFileHeaderType *>(p_RtlPcToFileHeader);
      return STATUS_SUCCESS;
    }
  }

#pragma warning(push)
#pragma warning(disable : 28175)
  auto module = static_cast<LdrDataTableEntry *>(driver_object->DriverSection);
#pragma warning(pop)

  g_utilp_PsLoadedModuleList = module->in_load_order_links.Flink;
  g_utilp_RtlPcToFileHeader = UtilpUnsafePcToFileHeader;
  return STATUS_SUCCESS;
}

/*  */
PVOID UtilpGetNTHeaderInfo(PVOID *base_of_image, ULONG &size_of_image) {
  return UtilpUnsafeGetNTFileHeaderAdvanced(base_of_image, size_of_image);
}

PVOID UtilpGetModuleBasenSize(__in PVOID pc_value, PVOID *base_of_image, ULONG &size_of_image) {
  return UtilpUnsafePcToFileHeaderAdvanced(pc_value, base_of_image, size_of_image);
}

/*  */
PVOID UtilpGetAnyModuleHeaderInfo(UNICODE_STRING str, PVOID *base_of_image, ULONG &size_of_image) {
  return UtilpUnsafeGetAnyModuleFileHeader(str, base_of_image, size_of_image);
}


_Use_decl_annotations_ static PVOID NTAPI
UtilpUnsafeGetAnyModuleFileHeader(UNICODE_STRING &str, PVOID *base_of_image, ULONG &size_of_image) {

  PVOID st_base = NULL;
  auto st_size = 0;
  const auto head = g_utilp_PsLoadedModuleList;
  for (auto current = head->Flink; current != head; current = current->Flink) {
    const auto module =
      CONTAINING_RECORD(current, LdrDataTableEntry, in_load_order_links);

    if (TRUE == FsRtlIsNameInExpression(&str, &module->full_dll_name, TRUE, nullptr)) {
      st_base = module->dll_base;
      st_size = module->size_of_image;

      *base_of_image = st_base;
      size_of_image = st_size;
      return *base_of_image;
    }

  }
  return nullptr;
}

_Use_decl_annotations_ static PVOID NTAPI
UtilpUnsafeGetNTFileHeaderAdvanced(PVOID *base_of_image, ULONG &size_of_image) {

  static PVOID st_base = NULL;
  static auto st_size = 0;

  if (!st_base && !st_size) {
    static UNICODE_STRING nt_os_krnl[] = {
      RTL_CONSTANT_STRING(L"*\\NTOSKRNL.EXE"),
      RTL_CONSTANT_STRING(L"*\\NTKRNLPA.EXE"),
      RTL_CONSTANT_STRING(L"*\\NTKRNLMP.EXE"),
      RTL_CONSTANT_STRING(L"*\\NTKRNLAMP.EXE")
    };

    const auto head = g_utilp_PsLoadedModuleList;
    for (auto current = head->Flink; current != head; current = current->Flink) {
      const auto module =
        CONTAINING_RECORD(current, LdrDataTableEntry, in_load_order_links);

      for (auto& expression : nt_os_krnl) {
        if (TRUE == FsRtlIsNameInExpression(&expression, &module->full_dll_name, TRUE, nullptr)) {
          st_base = module->dll_base;
          st_size = module->size_of_image;

          *base_of_image = st_base;
          size_of_image = st_size;
          return *base_of_image;
        }
      }
    }
  }

  *base_of_image = st_base;
  size_of_image = st_size;
  return *base_of_image;
}

_Use_decl_annotations_ static PVOID NTAPI
UtilpUnsafePcToFileHeaderAdvanced(__in PVOID pc_value, PVOID *base_of_image, ULONG &size_of_image) {
  if (pc_value < MmSystemRangeStart) {
    return nullptr;
  }

  const auto head = g_utilp_PsLoadedModuleList;
  for (auto current = head->Flink; current != head; current = current->Flink) {
    const auto module =
      CONTAINING_RECORD(current, LdrDataTableEntry, in_load_order_links);
    const auto driver_end = reinterpret_cast<void *>(
      reinterpret_cast<ULONG_PTR>(module->dll_base) + module->size_of_image);
    if (UtilIsInBounds(pc_value, module->dll_base, driver_end)) {
      *base_of_image = module->dll_base;
      size_of_image = module->size_of_image;
      return module->dll_base;
    }
  }
  return nullptr;
}


// A fake RtlPcToFileHeader without acquiring PsLoadedModuleSpinLock. Thus, it
// is unsafe and should be updated if we can locate PsLoadedModuleSpinLock.
_Use_decl_annotations_ static PVOID NTAPI
UtilpUnsafePcToFileHeader(PVOID pc_value, PVOID *base_of_image) {
  if (pc_value < MmSystemRangeStart) {
    return nullptr;
  }

  const auto head = g_utilp_PsLoadedModuleList;
  for (auto current = head->Flink; current != head; current = current->Flink) {
    const auto module =
        CONTAINING_RECORD(current, LdrDataTableEntry, in_load_order_links);
    const auto driver_end = reinterpret_cast<void *>(
        reinterpret_cast<ULONG_PTR>(module->dll_base) + module->size_of_image);
    if (UtilIsInBounds(pc_value, module->dll_base, driver_end)) {
      *base_of_image = module->dll_base;
      return module->dll_base;
    }
  }
  return nullptr;
}

// A wrapper of RtlPcToFileHeader
_Use_decl_annotations_ void *UtilPcToFileHeader(void *pc_value) {
  void *base = nullptr;
  return g_utilp_RtlPcToFileHeader(pc_value, &base);
}

// Initializes the physical memory ranges
_Use_decl_annotations_ static NTSTATUS UtilpInitializePhysicalMemoryRanges() {
  PAGED_CODE();

  const auto ranges = UtilpBuildPhysicalMemoryRanges();
  if (!ranges) {
    return STATUS_UNSUCCESSFUL;
  }

  g_utilp_physical_memory_ranges = ranges;

  for (auto i = 0ul; i < ranges->number_of_runs; ++i) {
    const auto base_addr =
        static_cast<ULONG64>(ranges->run[i].base_page) * PAGE_SIZE;
    HYPERPLATFORM_LOG_DEBUG("Physical Memory Range: %016llx - %016llx",
                            base_addr,
                            base_addr + ranges->run[i].page_count * PAGE_SIZE);
  }

  const auto pm_size =
      static_cast<ULONG64>(ranges->number_of_pages) * PAGE_SIZE;
  HYPERPLATFORM_LOG_DEBUG("Physical Memory Total: %llu KB", pm_size / 1024);

  return STATUS_SUCCESS;
}

// Builds the physical memory ranges
_Use_decl_annotations_ static PhysicalMemoryDescriptor *
UtilpBuildPhysicalMemoryRanges() {
  PAGED_CODE();

  const auto pm_ranges = MmGetPhysicalMemoryRanges();
  if (!pm_ranges) {
    return nullptr;
  }

  PFN_COUNT number_of_runs = 0;
  PFN_NUMBER number_of_pages = 0;
  for (/**/; /**/; ++number_of_runs) {
    const auto range = &pm_ranges[number_of_runs];
    if (!range->BaseAddress.QuadPart && !range->NumberOfBytes.QuadPart) {
      break;
    }
    number_of_pages +=
        static_cast<PFN_NUMBER>(BYTES_TO_PAGES(range->NumberOfBytes.QuadPart));
  }
  if (number_of_runs == 0) {
    ExFreePoolWithTag(pm_ranges, 'hPmM');
    return nullptr;
  }

  const auto memory_block_size =
      sizeof(PhysicalMemoryDescriptor) +
      sizeof(PhysicalMemoryRun) * (number_of_runs - 1);
  const auto pm_block =
      static_cast<PhysicalMemoryDescriptor *>(ExAllocatePoolWithTag(
          NonPagedPool, memory_block_size, kHyperPlatformCommonPoolTag));
  if (!pm_block) {
    ExFreePoolWithTag(pm_ranges, 'hPmM');
    return nullptr;
  }
  RtlZeroMemory(pm_block, memory_block_size);

  pm_block->number_of_runs = number_of_runs;
  pm_block->number_of_pages = number_of_pages;

  for (auto run_index = 0ul; run_index < number_of_runs; run_index++) {
    auto current_run = &pm_block->run[run_index];
    auto current_block = &pm_ranges[run_index];
    current_run->base_page = static_cast<ULONG_PTR>(
        UtilPfnFromPa(current_block->BaseAddress.QuadPart));
    current_run->page_count = static_cast<ULONG_PTR>(
        BYTES_TO_PAGES(current_block->NumberOfBytes.QuadPart));
  }

  ExFreePoolWithTag(pm_ranges, 'hPmM');
  return pm_block;
}

// Returns the physical memory ranges
/*_Use_decl_annotations_*/ const PhysicalMemoryDescriptor *
UtilGetPhysicalMemoryRanges() {
  return g_utilp_physical_memory_ranges;
}

// Execute a given callback routine on all processors in PASSIVE_LEVEL. Returns
// STATUS_SUCCESS when all callback returned STATUS_SUCCESS as well. When
// one of callbacks returns anything but STATUS_SUCCESS, this function stops
// to call remaining callbacks and returns the value.
_Use_decl_annotations_ NTSTATUS
UtilForEachProcessor(NTSTATUS (*callback_routine)(void *), void *context) {
  PAGED_CODE();

  const auto number_of_processors =
      KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
  for (ULONG processor_index = 0; processor_index < number_of_processors;
       processor_index++) {
    PROCESSOR_NUMBER processor_number = {};
    auto status =
        KeGetProcessorNumberFromIndex(processor_index, &processor_number);
    if (!NT_SUCCESS(status)) {
      return status;
    }

    // Switch the current processor
    GROUP_AFFINITY affinity = {};
    affinity.Group = processor_number.Group;
    affinity.Mask = 1ull << processor_number.Number;
    GROUP_AFFINITY previous_affinity = {};
    KeSetSystemGroupAffinityThread(&affinity, &previous_affinity);

    // Execute callback
    status = callback_routine(context);

    KeRevertToUserGroupAffinityThread(&previous_affinity);
    if (!NT_SUCCESS(status)) {
      return status;
    }
  }
  return STATUS_SUCCESS;
}

// Queues a given DPC routine on all processors. Returns STATUS_SUCCESS when DPC
// is queued for all processors.
_Use_decl_annotations_ NTSTATUS
UtilForEachProcessorDpc(PKDEFERRED_ROUTINE deferred_routine, void *context) {
  const auto number_of_processors =
      KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
  for (ULONG processor_index = 0; processor_index < number_of_processors;
       processor_index++) {
    PROCESSOR_NUMBER processor_number = {};
    auto status =
        KeGetProcessorNumberFromIndex(processor_index, &processor_number);
    if (!NT_SUCCESS(status)) {
      return status;
    }

    const auto dpc = static_cast<PRKDPC>(ExAllocatePoolWithTag(
        NonPagedPool, sizeof(KDPC), kHyperPlatformCommonPoolTag));
    if (!dpc) {
      return STATUS_MEMORY_NOT_ALLOCATED;
    }
    KeInitializeDpc(dpc, deferred_routine, context);
    KeSetImportanceDpc(dpc, HighImportance);
    status = KeSetTargetProcessorDpcEx(dpc, &processor_number);
    if (!NT_SUCCESS(status)) {
      ExFreePoolWithTag(dpc, kHyperPlatformCommonPoolTag);
      return status;
    }
    KeInsertQueueDpc(dpc, nullptr, nullptr);
  }
  return STATUS_SUCCESS;
}

// Sleep the current thread's execution for Millisecond milliseconds.
_Use_decl_annotations_ NTSTATUS UtilSleep(LONG Millisecond) {
  PAGED_CODE();

  LARGE_INTEGER interval = {};
  interval.QuadPart = -(10000 * Millisecond);  // msec
  return KeDelayExecutionThread(KernelMode, FALSE, &interval);
}

// memmem().
_Use_decl_annotations_ void *UtilMemMem(const void *search_base,
                                        SIZE_T search_size, const void *pattern,
                                        SIZE_T pattern_size) {
  if (pattern_size > search_size) {
    return nullptr;
  }
  auto base = static_cast<const char *>(search_base);
  for (SIZE_T i = 0; i <= search_size - pattern_size; i++) {
    if (RtlCompareMemory(pattern, &base[i], pattern_size) == pattern_size) {
      return const_cast<char *>(&base[i]);
    }
  }
  return nullptr;
}

// A wrapper of MmGetSystemRoutineAddress
_Use_decl_annotations_ void *UtilGetSystemProcAddress(
    const wchar_t *proc_name) {
  PAGED_CODE();

  UNICODE_STRING proc_name_U = {};
  RtlInitUnicodeString(&proc_name_U, proc_name);
  return MmGetSystemRoutineAddress(&proc_name_U);
}

// Returns true when a system is on the x86 PAE mode
/*_Use_decl_annotations_*/ bool UtilIsX86Pae() {
  return (!IsX64() && Cr4{__readcr4()}.fields.pae);
}

// Return true if the given address is accessible.
_Use_decl_annotations_ bool UtilIsAccessibleAddress(void *address) {
  if (!UtilpIsCanonicalFormAddress(address)) {
    return false;
  }

// UtilpAddressToPxe, UtilpAddressToPpe defined for x64
#if defined(_AMD64_)
  const auto pxe = UtilpAddressToPxe(address);
  const auto ppe = UtilpAddressToPpe(address);
  if (!pxe->valid || !ppe->valid) {
    return false;
  }
#endif

  const auto pde = UtilpAddressToPde(address);
  const auto pte = UtilpAddressToPte(address);
  if (!pde->valid) {
    return false;
  }
  if (pde->large_page) {
    return true;  // A large page is always memory resident
  }
  if (!pte || !pte->valid) {
    return false;
  }
  return true;
}


// Return true if the given address is executable.
_Use_decl_annotations_ bool UtilIsExecutableAddress(void *address) {
  if (!UtilpIsCanonicalFormAddress(address)) {
    return false;
  }

  // Instruction fetch is prohibited if the XD flag is 1 in any paging-structure
  // entry controlling the translation.
  // See: Determination of Access Rights
  if (IsX64()) {
    const auto pxe = UtilpAddressToPxe(address);
    const auto ppe = UtilpAddressToPpe(address);
    if (!pxe->valid || pxe->no_execute) {
      return false;
    }
    if (!ppe->valid || ppe->no_execute) {
      return false;
    }
  }

  const auto pde = UtilpAddressToPde(address);
  const auto pte = UtilpAddressToPte(address);
  if (!pde->valid) {
    return false;
  }
  if (pde->large_page) {
    return pde->no_execute == false;
  }
  if (!pte || !pte->valid) {
    return false;
  }

  return pte->no_execute == false;
}

// Checks whether the address is the canonical address
_Use_decl_annotations_ static bool UtilpIsCanonicalFormAddress(void *address) {
  if (!IsX64()) {
    return true;
  } else {
    return !UtilIsInBounds(0x0000800000000000ull, 0xffff7fffffffffffull,
                           reinterpret_cast<ULONG64>(address));
  }
}

#if defined(_AMD64_)
// Return an address of PXE
_Use_decl_annotations_ static HardwarePte *UtilpAddressToPxe(
    const void *address) {
  const auto addr = reinterpret_cast<ULONG_PTR>(address);
  const auto pxe_index = (addr >> g_utilp_pxi_shift) & g_utilp_pxi_mask;
  const auto offset = pxe_index * sizeof(HardwarePte);
  return reinterpret_cast<HardwarePte *>(g_utilp_pxe_base + offset);
}

// Return an address of PPE
_Use_decl_annotations_ static HardwarePte *UtilpAddressToPpe(
    const void *address) {
  const auto addr = reinterpret_cast<ULONG_PTR>(address);
  const auto ppe_index = (addr >> g_utilp_ppi_shift) & g_utilp_ppi_mask;
  const auto offset = ppe_index * sizeof(HardwarePte);
  return reinterpret_cast<HardwarePte *>(g_utilp_ppe_base + offset);
}
#endif

// Return an address of PDE
_Use_decl_annotations_ static HardwarePte *UtilpAddressToPde(
    const void *address) {
  const auto addr = reinterpret_cast<ULONG_PTR>(address);
  const auto pde_index = (addr >> g_utilp_pdi_shift) & g_utilp_pdi_mask;
  const auto offset = pde_index * sizeof(HardwarePte);
  return reinterpret_cast<HardwarePte *>(g_utilp_pde_base + offset);
}

// Return an address of PTE
_Use_decl_annotations_ static HardwarePte *UtilpAddressToPte(
    const void *address) {
  const auto addr = reinterpret_cast<ULONG_PTR>(address);
  const auto pte_index = (addr >> g_utilp_pti_shift) & g_utilp_pti_mask;
  const auto offset = pte_index * sizeof(HardwarePte);
  return reinterpret_cast<HardwarePte *>(g_utilp_pte_base + offset);
}

// VA -> PA
_Use_decl_annotations_ ULONG64 UtilPaFromVa(void *va) {
  //  - https://www.codemachine.com/article_protopte.html
  //  - http://residentmemory.blogspot.com/2011/06/prototype-pte-and-sharing-memory.html
  //  - https://courses.cs.washington.edu/courses/csep590a/18au/schedule/Windows%202000%20Performance%20Guide.pdf
  //  - https://github.com/sikuner/book-code/blob/7f4883d9030d553bc6bcfa3da685e34789839900/hack-tools/A-Protect/Driver/FuncAddrValid.c

  const auto pa = MmGetPhysicalAddress(va);
  if (pa.QuadPart) {
    return pa.QuadPart;
  }

  if (!UtilpIsCanonicalFormAddress(va)) {
    return 0;
  }

  if (IsX64()) {
    const auto pxe = UtilpAddressToPxe(va);
    const auto ppe = UtilpAddressToPpe(va);
    if (!pxe->valid || !ppe->valid) {
      return 0;
    }
  }

  const auto pde = UtilpAddressToPde(va);
  const auto pte = UtilpAddressToPte(va);
  if (!pde || !pde->valid) {
    return 0;
  }
  if (!pte || !pte->valid) {
    return 0;
  }

  const auto mmpte = reinterpret_cast<MmPte *>(pte);
  // PTE's can be subdivided into three other categories: Invalid, Prototype and System.
  if (!mmpte->Hard.valid) {
    //  When the PTE Present bit 0 is not set,the hardware ignores the remaining contents of the PTE
    //  Windows uses the empty space in an invalid PTE to store 
    //  essential information about where a paged-out page can be located on the paging file.
    return 0;
  }
  if (!mmpte->Proto.Prototype) {
    if (mmpte->Trans.Transition) {
      // Transition PTE
      const auto pa_base = mmpte->Hard.page_frame_number << PAGE_SHIFT;
      return pa_base + BYTE_OFFSET(va);
    }
    if (mmpte->Soft.PageFileHigh) {
      // PageFile PTE
    } else {
      // VAD PTE
    }
    return 0;

  } else if (mmpte->Proto.ProtoAddress != 0xFFFFFFFF0000) {
    // Prototype PTE
    // Prototype PTE describes 5 states for shared page:
    // Active / valid, Transition, Demand zero, Page file, Mapped file

    const auto prototype_pte = reinterpret_cast<MmPte *>(
        mmpte->Proto.ProtoAddress | 0xFFFF000000000000);

    if (prototype_pte->Hard.valid) {
      // Valid PTE
      const auto pa_base = prototype_pte->Hard.page_frame_number << PAGE_SHIFT;
      return pa_base + BYTE_OFFSET(va);
    }
    if (prototype_pte->Proto.Prototype) {
      // File mapping PTE
      return 0;
    }
    if (!prototype_pte->Trans.Transition) {
      // Pagefile PTE
      return 0;
    }
    // Transition PTE
    const auto pa_base = prototype_pte->Hard.page_frame_number << PAGE_SHIFT;
    return pa_base + BYTE_OFFSET(va);

  } else {
    // VAD PTE
    HYPERPLATFORM_COMMON_DBG_BREAK();
    return 0;
  }
}

// VA -> PFN
_Use_decl_annotations_ PFN_NUMBER UtilPfnFromVa(void *va) {
  return UtilPfnFromPa(UtilPaFromVa(va));
}

// PA -> PFN
_Use_decl_annotations_ PFN_NUMBER UtilPfnFromPa(ULONG64 pa) {
  return static_cast<PFN_NUMBER>(pa >> PAGE_SHIFT);
}

// PA -> VA
_Use_decl_annotations_ void *UtilVaFromPa(ULONG64 pa) {
  PHYSICAL_ADDRESS pa2 = {};
  pa2.QuadPart = pa;
  return MmGetVirtualForPhysical(pa2);
}

// PNF -> PA
_Use_decl_annotations_ ULONG64 UtilPaFromPfn(PFN_NUMBER pfn) {
  return static_cast<ULONG64>(pfn) << PAGE_SHIFT;
}

// PFN -> VA
_Use_decl_annotations_ void *UtilVaFromPfn(PFN_NUMBER pfn) {
  return UtilVaFromPa(UtilPaFromPfn(pfn));
}

// Allocates continuous physical memory
_Use_decl_annotations_ void *UtilAllocateContiguousMemory(
    SIZE_T number_of_bytes) {
  PHYSICAL_ADDRESS highest_acceptable_address = {};
  highest_acceptable_address.QuadPart = -1;
  if (g_utilp_MmAllocateContiguousNodeMemory) {
    // Allocate NX physical memory
    PHYSICAL_ADDRESS lowest_acceptable_address = {};
    PHYSICAL_ADDRESS boundary_address_multiple = {};
    return g_utilp_MmAllocateContiguousNodeMemory(
        number_of_bytes, lowest_acceptable_address, highest_acceptable_address,
        boundary_address_multiple, PAGE_READWRITE, MM_ANY_NODE_OK);
  } else {
#pragma warning(push)
#pragma warning(disable : 30029)
    return MmAllocateContiguousMemory(number_of_bytes,
                                      highest_acceptable_address);
#pragma warning(pop)
  }
}

// Frees an address allocated by UtilAllocateContiguousMemory()
_Use_decl_annotations_ void UtilFreeContiguousMemory(void *base_address) {
  MmFreeContiguousMemory(base_address);
}

// Executes VMCALL
_Use_decl_annotations_ NTSTATUS UtilVmCall(HypercallNumber hypercall_number,
                                           void *context) {
  __try {
    const auto vmx_status = static_cast<VmxStatus>(
        AsmVmxCall(static_cast<ULONG>(hypercall_number), context));
    return (vmx_status == VmxStatus::kOk) ? STATUS_SUCCESS
                                          : STATUS_UNSUCCESSFUL;

#pragma prefast(suppress : __WARNING_EXCEPTIONEXECUTEHANDLER, "Catch all.");
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    const auto status = GetExceptionCode();
    HYPERPLATFORM_COMMON_DBG_BREAK();
    HYPERPLATFORM_LOG_WARN_SAFE("Exception thrown (code %08x)", status);
    return status;
  }
}

// Debug prints registers
_Use_decl_annotations_ void UtilDumpGpRegisters(const AllRegisters *all_regs,
                                                ULONG_PTR stack_pointer) {
  const auto current_irql = KeGetCurrentIrql();
  if (current_irql < DISPATCH_LEVEL) {
    KeRaiseIrqlToDpcLevel();
  }

#if defined(_AMD64_)
  HYPERPLATFORM_LOG_DEBUG_SAFE(
      "Context at %p: "
      "rax= %016Ix rbx= %016Ix rcx= %016Ix "
      "rdx= %016Ix rsi= %016Ix rdi= %016Ix "
      "rsp= %016Ix rbp= %016Ix "
      " r8= %016Ix  r9= %016Ix r10= %016Ix "
      "r11= %016Ix r12= %016Ix r13= %016Ix "
      "r14= %016Ix r15= %016Ix efl= %08Ix",
      _ReturnAddress(), all_regs->gp.ax, all_regs->gp.bx, all_regs->gp.cx,
      all_regs->gp.dx, all_regs->gp.si, all_regs->gp.di, stack_pointer,
      all_regs->gp.bp, all_regs->gp.r8, all_regs->gp.r9, all_regs->gp.r10,
      all_regs->gp.r11, all_regs->gp.r12, all_regs->gp.r13, all_regs->gp.r14,
      all_regs->gp.r15, all_regs->flags.all);
#else
  HYPERPLATFORM_LOG_DEBUG_SAFE(
      "Context at %p: "
      "eax= %08Ix ebx= %08Ix ecx= %08Ix "
      "edx= %08Ix esi= %08Ix edi= %08Ix "
      "esp= %08Ix ebp= %08Ix efl= %08x",
      _ReturnAddress(), all_regs->gp.ax, all_regs->gp.bx, all_regs->gp.cx,
      all_regs->gp.dx, all_regs->gp.si, all_regs->gp.di, stack_pointer,
      all_regs->gp.bp, all_regs->flags.all);
#endif

  if (current_irql < DISPATCH_LEVEL) {
    KeLowerIrql(current_irql);
  }
}

// Reads natural-width VMCS
_Use_decl_annotations_ ULONG_PTR UtilVmRead(VmcsField field) {
  size_t field_value = 0;
  const auto vmx_status = static_cast<VmxStatus>(
      __vmx_vmread(static_cast<size_t>(field), &field_value));
  if (vmx_status != VmxStatus::kOk) {
    HYPERPLATFORM_COMMON_BUG_CHECK(
        HyperPlatformBugCheck::kCriticalVmxInstructionFailure,
        static_cast<ULONG_PTR>(vmx_status), static_cast<ULONG_PTR>(field), 0);
  }
  return field_value;
}

// Reads 64bit-width VMCS
_Use_decl_annotations_ ULONG64 UtilVmRead64(VmcsField field) {
#if defined(_AMD64_)
  return UtilVmRead(field);
#else
  // Only 64bit fields should be given on x86 because it access field + 1 too.
  // Also, the field must be even number.
  NT_ASSERT(UtilIsInBounds(field, VmcsField::kIoBitmapA,
                           VmcsField::kHostIa32PerfGlobalCtrlHigh));
  NT_ASSERT((static_cast<ULONG>(field) % 2) == 0);

  ULARGE_INTEGER value64 = {};
  value64.LowPart = UtilVmRead(field);
  value64.HighPart =
      UtilVmRead(static_cast<VmcsField>(static_cast<ULONG>(field) + 1));
  return value64.QuadPart;
#endif
}

// Writes natural-width VMCS
_Use_decl_annotations_ VmxStatus UtilVmWrite(VmcsField field,
                                             ULONG_PTR field_value) {
  return static_cast<VmxStatus>(
      __vmx_vmwrite(static_cast<size_t>(field), field_value));
}

// Writes 64bit-width VMCS
_Use_decl_annotations_ VmxStatus UtilVmWrite64(VmcsField field,
                                               ULONG64 field_value) {
#if defined(_AMD64_)
  return UtilVmWrite(field, field_value);
#else
  // Only 64bit fields should be given on x86 because it access field + 1 too.
  // Also, the field must be even number.
  NT_ASSERT(UtilIsInBounds(field, VmcsField::kIoBitmapA,
                           VmcsField::kHostIa32PerfGlobalCtrlHigh));
  NT_ASSERT((static_cast<ULONG>(field) % 2) == 0);

  ULARGE_INTEGER value64 = {};
  value64.QuadPart = field_value;
  const auto vmx_status = UtilVmWrite(field, value64.LowPart);
  if (vmx_status != VmxStatus::kOk) {
    return vmx_status;
  }
  return UtilVmWrite(static_cast<VmcsField>(static_cast<ULONG>(field) + 1),
                     value64.HighPart);
#endif
}

// Reads natural-width MSR
_Use_decl_annotations_ ULONG_PTR UtilReadMsr(Msr msr) {
  return static_cast<ULONG_PTR>(__readmsr(static_cast<unsigned long>(msr)));
}

// Reads 64bit-width MSR
_Use_decl_annotations_ ULONG64 UtilReadMsr64(Msr msr) {
  return __readmsr(static_cast<unsigned long>(msr));
}

// Writes natural-width MSR
_Use_decl_annotations_ void UtilWriteMsr(Msr msr, ULONG_PTR value) {
  __writemsr(static_cast<unsigned long>(msr), value);
}

// Writes 64bit-width MSR
_Use_decl_annotations_ void UtilWriteMsr64(Msr msr, ULONG64 value) {
  __writemsr(static_cast<unsigned long>(msr), value);
}

// Executes the INVEPT instruction and invalidates EPT entry cache
/*_Use_decl_annotations_*/ VmxStatus UtilInveptGlobal() {
  InvEptDescriptor desc = {};
  return static_cast<VmxStatus>(
      AsmInvept(InvEptType::kGlobalInvalidation, &desc));
}

// Executes the INVVPID instruction (type 0)
_Use_decl_annotations_ VmxStatus UtilInvvpidIndividualAddress(USHORT vpid,
                                                              void *address) {
  InvVpidDescriptor desc = {};
  desc.vpid = vpid;
  desc.linear_address = reinterpret_cast<ULONG64>(address);
  return static_cast<VmxStatus>(
      AsmInvvpid(InvVpidType::kIndividualAddressInvalidation, &desc));
}

// Executes the INVVPID instruction (type 1)
_Use_decl_annotations_ VmxStatus UtilInvvpidSingleContext(USHORT vpid) {
  InvVpidDescriptor desc = {};
  desc.vpid = vpid;
  return static_cast<VmxStatus>(
      AsmInvvpid(InvVpidType::kSingleContextInvalidation, &desc));
}

// Executes the INVVPID instruction (type 2)
/*_Use_decl_annotations_*/ VmxStatus UtilInvvpidAllContext() {
  InvVpidDescriptor desc = {};
  return static_cast<VmxStatus>(
      AsmInvvpid(InvVpidType::kAllContextInvalidation, &desc));
}

// Executes the INVVPID instruction (type 3)
_Use_decl_annotations_ VmxStatus
UtilInvvpidSingleContextExceptGlobal(USHORT vpid) {
  InvVpidDescriptor desc = {};
  desc.vpid = vpid;
  return static_cast<VmxStatus>(
      AsmInvvpid(InvVpidType::kSingleContextInvalidationExceptGlobal, &desc));
}

// Loads the PDPTE registers from CR3 to VMCS
_Use_decl_annotations_ void UtilLoadPdptes(ULONG_PTR cr3_value) {
  const auto current_cr3 = __readcr3();

  // Have to load cr3 to make UtilPfnFromVa() work properly.
  __writecr3(cr3_value);

  // Gets PDPTEs form CR3
  PdptrRegister pd_pointers[4] = {};
  for (auto i = 0ul; i < 4; ++i) {
    const auto pd_addr = g_utilp_pde_base + i * PAGE_SIZE;
    pd_pointers[i].fields.present = true;
    pd_pointers[i].fields.page_directory_pa =
        UtilPfnFromVa(reinterpret_cast<void *>(pd_addr));
  }

  __writecr3(current_cr3);
  UtilVmWrite64(VmcsField::kGuestPdptr0, pd_pointers[0].all);
  UtilVmWrite64(VmcsField::kGuestPdptr1, pd_pointers[1].all);
  UtilVmWrite64(VmcsField::kGuestPdptr2, pd_pointers[2].all);
  UtilVmWrite64(VmcsField::kGuestPdptr3, pd_pointers[3].all);
}

// Does RtlCopyMemory safely even if destination is a read only region
_Use_decl_annotations_ NTSTATUS UtilForceCopyMemory(void *destination,
                                                    const void *source,
                                                    SIZE_T length) {
  auto mdl = IoAllocateMdl(destination, static_cast<ULONG>(length), FALSE,
                           FALSE, nullptr);
  if (!mdl) {
    return STATUS_INSUFFICIENT_RESOURCES;
  }
  MmBuildMdlForNonPagedPool(mdl);

#pragma warning(push)
#pragma warning(disable : 28145)
  // Following MmMapLockedPagesSpecifyCache() call causes bug check in case
  // you are using Driver Verifier. The reason is explained as followings:
  //
  // A driver must not try to create more than one system-address-space
  // mapping for an MDL. Additionally, because an MDL that is built by the
  // MmBuildMdlForNonPagedPool routine is already mapped to the system
  // address space, a driver must not try to map this MDL into the system
  // address space again by using the MmMapLockedPagesSpecifyCache routine.
  // -- MSDN
  //
  // This flag modification hacks Driver Verifier's check and prevent leading
  // bug check.
  mdl->MdlFlags &= ~MDL_SOURCE_IS_NONPAGED_POOL;
  mdl->MdlFlags |= MDL_PAGES_LOCKED;
#pragma warning(pop)

  const auto writable_dest =
      MmMapLockedPagesSpecifyCache(mdl, KernelMode, MmCached, nullptr, FALSE,
                                   NormalPagePriority | MdlMappingNoExecute);
  if (!writable_dest) {
    IoFreeMdl(mdl);
    return STATUS_INSUFFICIENT_RESOURCES;
  }
  RtlCopyMemory(writable_dest, source, length);
  MmUnmapLockedPages(writable_dest, mdl);
  IoFreeMdl(mdl);
  return STATUS_SUCCESS;
}

}  // extern "C"