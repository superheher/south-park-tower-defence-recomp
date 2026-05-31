# Variant A — host-runtime imports to implement (474 kernel/xam functions)

Enumerated from the recompiled `ppc/` (`__imp__<name>` symbols, excluding guest `sub_*`).
Behaviour reference for each: RexGlue's runtime `third_party/rexglue-sdk/src/` (it implements
these for this exact title) + Xenia/UnleashedRecomp. The recomp ABI: each is
`PPC_FUNC(__imp__<name>)` = `void(PPCContext& ctx, uint8_t* base)` (args in ctx.r3.., ret in ctx.r3).

## Count by module prefix
```
     44 Xam*
     30 Nt*
     28 Net*
     26 Ke*
     21 Rtl*
     20 Vd*
     11 Ex*
      7 XUsbcam*
      6 Xex*
      6 Mm*
      5 XAudio*
      5 Ob*
      4 XGet*
      2 XMA*
      2 _v*
      1 _xstart
      1 XNotifyPositionUI
      1 XNotifyGetNext
      1 XMsgStartIORequest
      1 XMsgInProcessCall
      1 XMsgCancelIORequest
      1 XeKeys*
      1 XeCryptSha
      1 XeCryptRotSumSha
      1 XeCryptBnQwBeSigVerify
      1 vswprintf
      1 vsprintf*
      1 swprintf
      1 sprintf*
      1 _snwprintf
      1 __savevmx_99
      1 __savevmx_98
      1 __savevmx_97
      1 __savevmx_96
      1 __savevmx_95
      1 __savevmx_94
      1 __savevmx_93
      1 __savevmx_92
      1 __savevmx_91
      1 __savevmx_90
      1 __savevmx_89
      1 __savevmx_88
      1 __savevmx_87
      1 __savevmx_86
      1 __savevmx_85
      1 __savevmx_84
      1 __savevmx_83
      1 __savevmx_82
      1 __savevmx_81
      1 __savevmx_80
      1 __savevmx_79
      1 __savevmx_78
      1 __savevmx_77
      1 __savevmx_76
      1 __savevmx_75
      1 __savevmx_74
      1 __savevmx_73
      1 __savevmx_72
      1 __savevmx_71
      1 __savevmx_70
      1 __savevmx_69
      1 __savevmx_68
      1 __savevmx_67
      1 __savevmx_66
      1 __savevmx_65
      1 __savevmx_64
      1 __savevmx_31
      1 __savevmx_30
      1 __savevmx_29
      1 __savevmx_28
      1 __savevmx_27
      1 __savevmx_26
      1 __savevmx_25
      1 __savevmx_24
      1 __savevmx_23
      1 __savevmx_22
      1 __savevmx_21
      1 __savevmx_20
      1 __savevmx_19
      1 __savevmx_18
      1 __savevmx_17
      1 __savevmx_16
      1 __savevmx_15
      1 __savevmx_14
      1 __savevmx_127
      1 __savevmx_126
      1 __savevmx_125
      1 __savevmx_124
      1 __savevmx_123
      1 __savevmx_122
      1 __savevmx_121
      1 __savevmx_120
      1 __savevmx_119
      1 __savevmx_118
      1 __savevmx_117
      1 __savevmx_116
      1 __savevmx_115
      1 __savevmx_114
      1 __savevmx_113
      1 __savevmx_112
      1 __savevmx_111
      1 __savevmx_110
      1 __savevmx_109
      1 __savevmx_108
      1 __savevmx_107
      1 __savevmx_106
      1 __savevmx_105
      1 __savevmx_104
      1 __savevmx_103
      1 __savevmx_102
      1 __savevmx_101
      1 __savevmx_100
      1 __savegprlr_31
      1 __savegprlr_30
      1 __savegprlr_29
      1 __savegprlr_28
      1 __savegprlr_27
      1 __savegprlr_26
      1 __savegprlr_25
      1 __savegprlr_24
      1 __savegprlr_23
      1 __savegprlr_22
      1 __savegprlr_21
      1 __savegprlr_20
      1 __savegprlr_19
      1 __savegprlr_18
      1 __savegprlr_17
      1 __savegprlr_16
      1 __savegprlr_15
      1 __savegprlr_14
      1 __savefpr_31
      1 __savefpr_30
      1 __savefpr_29
      1 __savefpr_28
      1 __savefpr_27
      1 __savefpr_26
      1 __savefpr_25
      1 __savefpr_24
      1 __savefpr_23
      1 __savefpr_22
      1 __savefpr_21
      1 __savefpr_20
      1 __savefpr_19
      1 __savefpr_18
      1 __savefpr_17
      1 __savefpr_16
      1 __savefpr_15
      1 __savefpr_14
      1 __restvmx_99
      1 __restvmx_98
      1 __restvmx_97
      1 __restvmx_96
      1 __restvmx_95
      1 __restvmx_94
      1 __restvmx_93
      1 __restvmx_92
      1 __restvmx_91
      1 __restvmx_90
      1 __restvmx_89
      1 __restvmx_88
      1 __restvmx_87
      1 __restvmx_86
      1 __restvmx_85
      1 __restvmx_84
      1 __restvmx_83
      1 __restvmx_82
      1 __restvmx_81
      1 __restvmx_80
      1 __restvmx_79
      1 __restvmx_78
      1 __restvmx_77
      1 __restvmx_76
      1 __restvmx_75
      1 __restvmx_74
      1 __restvmx_73
      1 __restvmx_72
      1 __restvmx_71
      1 __restvmx_70
      1 __restvmx_69
      1 __restvmx_68
      1 __restvmx_67
      1 __restvmx_66
      1 __restvmx_65
      1 __restvmx_64
      1 __restvmx_31
      1 __restvmx_30
      1 __restvmx_29
      1 __restvmx_28
      1 __restvmx_27
      1 __restvmx_26
      1 __restvmx_25
      1 __restvmx_24
      1 __restvmx_23
      1 __restvmx_22
      1 __restvmx_21
      1 __restvmx_20
      1 __restvmx_19
      1 __restvmx_18
      1 __restvmx_17
      1 __restvmx_16
      1 __restvmx_15
      1 __restvmx_14
      1 __restvmx_127
      1 __restvmx_126
      1 __restvmx_125
      1 __restvmx_124
      1 __restvmx_123
      1 __restvmx_122
      1 __restvmx_121
      1 __restvmx_120
      1 __restvmx_119
      1 __restvmx_118
      1 __restvmx_117
      1 __restvmx_116
      1 __restvmx_115
      1 __restvmx_114
      1 __restvmx_113
      1 __restvmx_112
      1 __restvmx_111
      1 __restvmx_110
      1 __restvmx_109
      1 __restvmx_108
      1 __restvmx_107
      1 __restvmx_106
      1 __restvmx_105
      1 __restvmx_104
      1 __restvmx_103
      1 __restvmx_102
      1 __restvmx_101
      1 __restvmx_100
      1 __restgprlr_31
      1 __restgprlr_30
      1 __restgprlr_29
      1 __restgprlr_28
      1 __restgprlr_27
      1 __restgprlr_26
      1 __restgprlr_25
      1 __restgprlr_24
      1 __restgprlr_23
      1 __restgprlr_22
      1 __restgprlr_21
      1 __restgprlr_20
      1 __restgprlr_19
      1 __restgprlr_18
      1 __restgprlr_17
      1 __restgprlr_16
      1 __restgprlr_15
      1 __restgprlr_14
      1 __restfpr_31
      1 __restfpr_30
      1 __restfpr_29
      1 __restfpr_28
      1 __restfpr_27
      1 __restfpr_26
      1 __restfpr_25
      1 __restfpr_24
      1 __restfpr_23
      1 __restfpr_22
      1 __restfpr_21
      1 __restfpr_20
      1 __restfpr_19
      1 __restfpr_18
      1 __restfpr_17
      1 __restfpr_16
      1 __restfpr_15
      1 __restfpr_14
      1 KiApcNormalRoutineNop
      1 KfReleaseSpinLock
      1 KfAcquireSpinLock
      1 Hal*
      1 Dbg*
      1 __C*
```

## Full list (474)
```
__C_specific_handler
DbgPrint
ExAcquireReadWriteLockExclusive
ExAcquireReadWriteLockShared
ExAllocatePoolTypeWithTag
ExAllocatePoolWithTag
ExCreateThread
ExFreePool
ExGetXConfigSetting
ExInitializeReadWriteLock
ExRegisterTitleTerminateNotification
ExReleaseReadWriteLock
ExTerminateThread
HalReturnToFirmware
KeAcquireSpinLockAtRaisedIrql
KeBugCheck
KeBugCheckEx
KeDelayExecutionThread
KeEnterCriticalRegion
KeGetCurrentProcessType
KeInitializeApc
KeInitializeDpc
KeInsertQueueApc
KeLeaveCriticalRegion
KeLockL2
KeQueryBasePriorityThread
KeQueryPerformanceFrequency
KeQuerySystemTime
KeReleaseSpinLockFromRaisedIrql
KeResetEvent
KeSetAffinityThread
KeSetBasePriorityThread
KeSetEvent
KeTlsAlloc
KeTlsFree
KeTlsGetValue
KeTlsSetValue
KeUnlockL2
KeWaitForMultipleObjects
KeWaitForSingleObject
KfAcquireSpinLock
KfReleaseSpinLock
KiApcNormalRoutineNop
MmAllocatePhysicalMemoryEx
MmFreePhysicalMemory
MmGetPhysicalAddress
MmMapIoSpace
MmQueryAddressProtect
MmQueryAllocationSize
NetDll_bind
NetDll_closesocket
NetDll_ioctlsocket
NetDll_recvfrom
NetDll_select
NetDll_setsockopt
NetDll_socket
NetDll_WSACleanup
NetDll_WSAGetLastError
NetDll_WSAGetOverlappedResult
NetDll_WSARecvFrom
NetDll_WSASendTo
NetDll_WSAStartup
NetDll_XNetCleanup
NetDll_XNetConnect
NetDll_XNetGetConnectStatus
NetDll_XNetGetOpt
NetDll_XNetGetTitleXnAddr
NetDll_XNetInAddrToXnAddr
NetDll_XNetQosListen
NetDll_XNetQosLookup
NetDll_XNetQosRelease
NetDll_XNetRandom
NetDll_XNetRegisterKey
NetDll_XNetReplaceKey
NetDll_XNetStartup
NetDll_XNetUnregisterInAddr
NetDll_XNetXnAddrToInAddr
NtAllocateVirtualMemory
NtCancelTimer
NtClearEvent
NtClose
NtCreateEvent
NtCreateFile
NtCreateMutant
NtCreateSemaphore
NtCreateTimer
NtDuplicateObject
NtFlushBuffersFile
NtFreeVirtualMemory
NtOpenFile
NtQueryDirectoryFile
NtQueryFullAttributesFile
NtQueryInformationFile
NtQueryVirtualMemory
NtQueryVolumeInformationFile
NtReadFile
NtReadFileScatter
NtReleaseMutant
NtReleaseSemaphore
NtResumeThread
NtSetEvent
NtSetInformationFile
NtSetTimerEx
NtSuspendThread
NtWaitForMultipleObjectsEx
NtWaitForSingleObjectEx
NtWriteFile
ObCreateSymbolicLink
ObDeleteSymbolicLink
ObDereferenceObject
ObReferenceObject
ObReferenceObjectByHandle
__restfpr_14
__restfpr_15
__restfpr_16
__restfpr_17
__restfpr_18
__restfpr_19
__restfpr_20
__restfpr_21
__restfpr_22
__restfpr_23
__restfpr_24
__restfpr_25
__restfpr_26
__restfpr_27
__restfpr_28
__restfpr_29
__restfpr_30
__restfpr_31
__restgprlr_14
__restgprlr_15
__restgprlr_16
__restgprlr_17
__restgprlr_18
__restgprlr_19
__restgprlr_20
__restgprlr_21
__restgprlr_22
__restgprlr_23
__restgprlr_24
__restgprlr_25
__restgprlr_26
__restgprlr_27
__restgprlr_28
__restgprlr_29
__restgprlr_30
__restgprlr_31
__restvmx_100
__restvmx_101
__restvmx_102
__restvmx_103
__restvmx_104
__restvmx_105
__restvmx_106
__restvmx_107
__restvmx_108
__restvmx_109
__restvmx_110
__restvmx_111
__restvmx_112
__restvmx_113
__restvmx_114
__restvmx_115
__restvmx_116
__restvmx_117
__restvmx_118
__restvmx_119
__restvmx_120
__restvmx_121
__restvmx_122
__restvmx_123
__restvmx_124
__restvmx_125
__restvmx_126
__restvmx_127
__restvmx_14
__restvmx_15
__restvmx_16
__restvmx_17
__restvmx_18
__restvmx_19
__restvmx_20
__restvmx_21
__restvmx_22
__restvmx_23
__restvmx_24
__restvmx_25
__restvmx_26
__restvmx_27
__restvmx_28
__restvmx_29
__restvmx_30
__restvmx_31
__restvmx_64
__restvmx_65
__restvmx_66
__restvmx_67
__restvmx_68
__restvmx_69
__restvmx_70
__restvmx_71
__restvmx_72
__restvmx_73
__restvmx_74
__restvmx_75
__restvmx_76
__restvmx_77
__restvmx_78
__restvmx_79
__restvmx_80
__restvmx_81
__restvmx_82
__restvmx_83
__restvmx_84
__restvmx_85
__restvmx_86
__restvmx_87
__restvmx_88
__restvmx_89
__restvmx_90
__restvmx_91
__restvmx_92
__restvmx_93
__restvmx_94
__restvmx_95
__restvmx_96
__restvmx_97
__restvmx_98
__restvmx_99
RtlCaptureContext
RtlCompareMemoryUlong
RtlEnterCriticalSection
RtlFillMemoryUlong
RtlFreeAnsiString
RtlImageXexHeaderField
RtlInitAnsiString
RtlInitializeCriticalSection
RtlInitializeCriticalSectionAndSpinCount
RtlInitUnicodeString
RtlLeaveCriticalSection
RtlMultiByteToUnicodeN
RtlNtStatusToDosError
RtlRaiseException
RtlTimeFieldsToTime
RtlTimeToTimeFields
RtlTryEnterCriticalSection
RtlUnicodeStringToAnsiString
RtlUnicodeToMultiByteN
RtlUnwind
RtlUpperChar
__savefpr_14
__savefpr_15
__savefpr_16
__savefpr_17
__savefpr_18
__savefpr_19
__savefpr_20
__savefpr_21
__savefpr_22
__savefpr_23
__savefpr_24
__savefpr_25
__savefpr_26
__savefpr_27
__savefpr_28
__savefpr_29
__savefpr_30
__savefpr_31
__savegprlr_14
__savegprlr_15
__savegprlr_16
__savegprlr_17
__savegprlr_18
__savegprlr_19
__savegprlr_20
__savegprlr_21
__savegprlr_22
__savegprlr_23
__savegprlr_24
__savegprlr_25
__savegprlr_26
__savegprlr_27
__savegprlr_28
__savegprlr_29
__savegprlr_30
__savegprlr_31
__savevmx_100
__savevmx_101
__savevmx_102
__savevmx_103
__savevmx_104
__savevmx_105
__savevmx_106
__savevmx_107
__savevmx_108
__savevmx_109
__savevmx_110
__savevmx_111
__savevmx_112
__savevmx_113
__savevmx_114
__savevmx_115
__savevmx_116
__savevmx_117
__savevmx_118
__savevmx_119
__savevmx_120
__savevmx_121
__savevmx_122
__savevmx_123
__savevmx_124
__savevmx_125
__savevmx_126
__savevmx_127
__savevmx_14
__savevmx_15
__savevmx_16
__savevmx_17
__savevmx_18
__savevmx_19
__savevmx_20
__savevmx_21
__savevmx_22
__savevmx_23
__savevmx_24
__savevmx_25
__savevmx_26
__savevmx_27
__savevmx_28
__savevmx_29
__savevmx_30
__savevmx_31
__savevmx_64
__savevmx_65
__savevmx_66
__savevmx_67
__savevmx_68
__savevmx_69
__savevmx_70
__savevmx_71
__savevmx_72
__savevmx_73
__savevmx_74
__savevmx_75
__savevmx_76
__savevmx_77
__savevmx_78
__savevmx_79
__savevmx_80
__savevmx_81
__savevmx_82
__savevmx_83
__savevmx_84
__savevmx_85
__savevmx_86
__savevmx_87
__savevmx_88
__savevmx_89
__savevmx_90
__savevmx_91
__savevmx_92
__savevmx_93
__savevmx_94
__savevmx_95
__savevmx_96
__savevmx_97
__savevmx_98
__savevmx_99
_snwprintf
sprintf
swprintf
VdCallGraphicsNotificationRoutines
VdEnableDisableClockGating
VdEnableRingBufferRPtrWriteBack
VdGetCurrentDisplayGamma
VdGetCurrentDisplayInformation
VdGetSystemCommandBuffer
VdInitializeEngines
VdInitializeRingBuffer
VdInitializeScalerCommandBuffer
VdIsHSIOTrainingSucceeded
VdPersistDisplay
VdQueryVideoFlags
VdQueryVideoMode
VdRetrainEDRAM
VdRetrainEDRAMWorker
VdSetDisplayMode
VdSetGraphicsInterruptCallback
VdSetSystemCommandBufferGpuIdentifierAddress
VdShutdownEngines
VdSwap
_vscwprintf
_vsnprintf
vsprintf
vswprintf
XamAlloc
XamContentClose
XamContentCreateEnumerator
XamContentCreateEx
XamContentGetLicenseMask
XamEnableInactivityProcessing
XamEnumerate
XamFree
XamGetExecutionId
XamGetSystemVersion
XamInputGetCapabilities
XamInputGetKeystrokeEx
XamInputGetState
XamInputSetState
XamLoaderLaunchTitle
XamLoaderTerminateTitle
XamNotifyCreateListener
XamParseGamerTileKey
XamReadTileToTexture
XamResetInactivity
XamSessionCreateHandle
XamSessionRefObjByHandle
XamShowAchievementsUI
XamShowFriendsUI
XamShowGamerCardUIForXUID
XamShowMarketplaceUI
XamShowMessageBoxUI
XamShowMessageBoxUIEx
XamShowPlayerReviewUI
XamShowSigninUI
XamUserAreUsersFriends
XamUserCheckPrivilege
XamUserCreateAchievementEnumerator
XamUserCreateStatsEnumerator
XamUserGetName
XamUserGetSigninInfo
XamUserGetSigninState
XamUserGetXUID
XamUserReadProfileSettings
XamUserWriteProfileSettings
XamVoiceClose
XamVoiceCreate
XamVoiceHeadsetPresent
XamVoiceSubmitPacket
XAudioGetSpeakerConfig
XAudioGetVoiceCategoryVolume
XAudioRegisterRenderDriverClient
XAudioSubmitRenderDriverFrame
XAudioUnregisterRenderDriverClient
XeCryptBnQwBeSigVerify
XeCryptRotSumSha
XeCryptSha
XeKeysGetKey
XexCheckExecutablePrivilege
XexGetModuleHandle
XexGetModuleSection
XexGetProcedureAddress
XexLoadImage
XexUnloadImage
XGetAVPack
XGetGameRegion
XGetLanguage
XGetVideoMode
XMACreateContext
XMAReleaseContext
XMsgCancelIORequest
XMsgInProcessCall
XMsgStartIORequest
XNotifyGetNext
XNotifyPositionUI
_xstart
XUsbcamCreate
XUsbcamDestroy
XUsbcamGetState
XUsbcamReadFrame
XUsbcamSetCaptureMode
XUsbcamSetConfig
XUsbcamSetView
```
