# CONFIG_DRM*
KCONFIG=	DRM_AMDGPU_CIK \
		DRM_AMDGPU_SI \
		DRM_AMD_DC \
		DRM_AMD_DC_SI \
		AMD_PMC \
		DRM_I915_FORCE_PROBE='"*"' \
		DRM_I915_REQUEST_TIMEOUT=20000 \
		DRM_I915_CAPTURE_ERROR \
		DRM_I915_USERFAULT_AUTOSUSPEND=250 \
		DRM_I915_STOP_TIMEOUT=100 \
		DRM_I915_PREEMPT_TIMEOUT=640 \
		DRM_I915_HEARTBEAT_INTERVAL=2500 \
		DRM_I915_TIMESLICE_DURATION=1 \
		DRM_I915_MAX_REQUEST_BUSYWAIT=8000 \
		DRM_I915_FENCE_TIMEOUT=10000 \
		DRM_MIPI_DSI \
		DRM_PANEL_ORIENTATION_QUIRKS

.if empty(NO_FBDEV)
KCONFIG+=	DRM_FBDEV_EMULATION \
		DRM_FBDEV_OVERALLOC=100
.endif

# non arch specific kconfig
KCONFIG+=	ARCH_HAVE_NMI_SAFE_CMPXCHG \
		BACKLIGHT_CLASS_DEVICE \
		DEBUG_FS \
		DMI \
		FB \
		MTRR \
		PCI \
		PM \
		SMP

.if ${MACHINE_CPUARCH} == "aarch64"
KCONFIG+=	64BIT \
		ACPI \
		ARM64
.endif

.if ${MACHINE_CPUARCH} == "i386" || ${MACHINE_CPUARCH} == "amd64"
KCONFIG+=	ACPI \
		ACPI_SLEEP \
		X86 \
		X86_PAT

.if ${MACHINE_CPUARCH} == "i386"
KCONFIG+=	AGP \
		DRM_LEGACY
.endif

.if ${MACHINE_CPUARCH} == "amd64"
KCONFIG+=	64BIT \
		AS_MOVNTDQA \
		COMPAT \
		X86_64

KCONFIG+=	DRM_AMD_DC_DCN \
		DRM_AMD_DC_DCN3_0 \
		DRM_AMD_DC_DCN3_01 \
		DRM_AMD_DC_DCN3_02 \
		DRM_AMD_DC_DCN3_1
.endif
.endif

.if ${MACHINE_ARCH:Mpowerpc64*} != ""
KCONFIG+=	64BIT \
		PPC64
		
# DCN is only compile-tested.
KCONFIG+=	DRM_AMD_DC_DCN \
		DRM_AMD_DC_DCN3_0
.endif

.if ${MACHINE_CPUARCH} == "riscv"
KCONFIG+=	RISCV

.if ${MACHINE_ARCH:Mriscv64*} != ""
KCONFIG+=	64BIT
.endif
.endif
