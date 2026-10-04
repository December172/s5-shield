// SPDX-License-Identifier: GPL-2.0
/*
 * s5_shield - keep the discrete GPU asleep on the way into ACPI S5.
 *
 * THE BUG (present in linux 7.2.8, drivers/pci/pci-driver.c):
 *
 *	static void pci_device_shutdown(struct device *dev)
 *	{
 *		...
 *		pm_runtime_resume(dev);          <-- unconditional
 *		if (drv && drv->shutdown)
 *			drv->shutdown(pci_dev);
 *	}
 *
 * device_shutdown() walks every device at poweroff and that resume brings the
 * NVIDIA dGPU back from D3cold to D0.  Once it is in D0 the platform never
 * cuts its rail, so the laptop sits in "S5" drawing ~20 W: dark, silent, warm
 * chassis, flat battery in the morning.  Windows on the same machine reaches
 * the real S5, which is why booting Windows "finishes" the shutdown.
 *
 * WHAT THIS DOES - one reboot notifier, nothing else.  kernel_power_off() runs
 * the reboot notifier chain in kernel_shutdown_prepare() *before*
 * device_shutdown() (v7.2.8: kernel/reboot.c:303-309), so by the time the PCI
 * core walks the device list:
 *
 *	(0) *optionally* (wait_ms=, default 0 = off) the module asks the listed
 *	    devices to go idle and waits for the display device to reach D3cold
 *	    with its bridge out of D0.  Off by default because it cannot do what
 *	    revision 1.3 added it for: see settle_before_arming();
 *	(a) runtime PM is disabled on the listed devices, so the
 *	    pm_runtime_resume() above bounces with -EACCES instead of powering
 *	    the GPU up (v7.2.8: drivers/base/power/runtime.c:796-808); and
 *	(b) drv->shutdown is NULL for the drivers named in `noshut`, so
 *	    nv_pci_shutdown() (and azx_shutdown(), which does MMIO through
 *	    remap_addr with no power-state check - sound/hda/controllers/intel.c
 *	    :2465-2474) cannot run against a device that is in D3cold.
 *
 * Plus two diagnostics, which never change any behaviour:
 *
 *	(c) at arming time, every listed device's runtime PM accounting, and
 *	    for the display device the exact reason it is not asleep yet
 *	    (rpm_blocker(), in the vocabulary of rpm_check_suspend_allowed());
 *	(d) a SYS_OFF_MODE_POWER_OFF_PREPARE observer that prints the FINAL
 *	    state of every listed device *after* device_shutdown() and before
 *	    the firmware is asked to power off - the one moment that decides
 *	    whether the rail gets cut.  See s5_shield_observe_final().
 *
 * (a) AND (b) MUST GO TOGETHER.  (a) alone leaves a driver callback running
 * against an unpowered device, which bus-locks the machine.  That is why this
 * revision refuses to do (a) for any device whose driver has a .shutdown
 * callback it is not allowed to null: half the job is not done, none of it is.
 * The single deliberate exception is a PCIe bridge, where only (a) is applied
 * because pcieport's .shutdown governs every port in the machine - that is the
 * combination measured clean on this exact model.
 *
 * FIX HISTORY
 *	1.0  first cut.  Two independent bugs, both of which made the module
 *	     refuse to load its own device list, so the fix was silently inert:
 *	     class_allowed() accepted PCI_CLASS_MULTIMEDIA_AUDIO (0x0401) while
 *	     this GPU's audio function is 0x0403, and it decoded the class with
 *	     "pdev->class >> 16" against 16-bit macros that only ">> 8" can
 *	     match.  Caught by review, not by testing.
 *	1.1  audio class accepted; (a)-without-(b) made impossible;
 *	     device_lock() around the driver read and the .shutdown write.
 *	1.2  class decoding moved into pci_class16() with the correct shift, and
 *	     every use goes through it; driver names are copied under
 *	     device_lock() instead of being read through a possibly-freed
 *	     pointer; devs=/noshut= tokens are trimmed on both sides; the
 *	     bridge check no longer sits on a wrong shift; shield_armed only
 *	     when something was actually done.
 *	1.3  the shield now WAITS for the display device to be asleep before it
 *	     disables runtime PM on it (wait_ms=, default 5000).  Revision 1.2
 *	     only ever *prevented a resume*: if the device was still in D0 when
 *	     the notifier ran, __pm_runtime_disable() pinned it there and the
 *	     rail stayed on for the whole of S5 - the fix would have been
 *	     silently useless in exactly the case it exists for.  The wait
 *	     nudged the runtime PM core with pm_request_idle() while it waited.
 *	     1.4 retired both, for the reasons written out in
 *	     settle_before_arming(): the nudge is refused exactly when it would
 *	     be needed, and the drop happens later, inside device_shutdown().
 *
 *	     bin/s5-shield-dryrun now reads the shift out of pci_class16() and the
 *	     accept-list out of class_allowed(), resolves both against the
 *	     running kernel's pci_ids.h and the live hardware, and refuses to let
 *	     install.sh proceed if they disagree.  That guard reproduces both
 *	     1.0/1.1 bugs; a mirror that cannot disagree with the source is
 *	     worthless, which is exactly how 1.0 shipped.
 *	1.4  the wait is off by default (wait_ms=0) and no longer depends on the
 *	     order of devs=: phase 1 asks every listed device to go idle, phase 2
 *	     polls, and "settled" means the display device is D3cold *and* no
 *	     listed bridge is still in D0 (the rail is released by the port, not
 *	     by the GPU).  Two read-only diagnostics were added: the runtime PM
 *	     accounting (and the blocking reason) at arming time, and a
 *	     POWER_OFF_PREPARE observer that records the state at the moment of
 *	     no return.  Rationale and measurement in settle_before_arming().
 *
 * WHAT IT DOES NOT DO
 *	- Nothing happens on reboot (SYS_RESTART) or halt (SYS_HALT), or at any
 *	  time while the system runs.  Only the poweroff path is touched.
 *	- No boot image, boot entry, kernel or initramfs is modified.
 *	- struct pci_driver is per-driver, not per-device, so nulling
 *	  ->shutdown affects every device of that driver (unavoidable); hence
 *	  the whitelist defaults to exactly "nvidia,snd_hda_intel".
 *	- Only the BDFs given on the command line are touched; each one is
 *	  class-checked at load time and again at poweroff, so a wrong address
 *	  (your NVMe) is refused rather than shielded.
 *
 * The shield is deliberately not undone by unloading the module: by the time
 * it is armed the system is already past the point of no return.  Only a
 * reboot clears it.
 *
 * Based on the diagnosis and method published by Anxo Calvo for the same
 * machine (HP OMEN 16-ap0xxx): https://github.com/AnxoCalvo/s5-poweroff-fix
 * This variant arms itself from the kernel's reboot notifier instead of a
 * userspace shutdown hook, so there is no ordering or read-only-/ problem.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/reboot.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/acpi.h>

#define S5_MAX_DEVS 8
#define S5_NAME_LEN 64
#define S5_POLL_MS 100

static char *devs = "";
module_param(devs, charp, 0444);
MODULE_PARM_DESC(devs, "Comma separated PCI BDFs to shield at poweroff (required, e.g. 0000:01:00.0,0000:01:00.1,0000:00:01.1)");

static char *noshut = "nvidia,snd_hda_intel";
module_param(noshut, charp, 0444);
MODULE_PARM_DESC(noshut, "Comma separated driver names whose .shutdown callback is nulled at poweroff");

/*
 * How long the notifier waits for the listed display device (and its bridge) to
 * settle before it disables runtime PM.  See settle_before_arming().
 *
 * DEFAULT 0: do not wait.  Revision 1.3 shipped 5000 and the wait cannot do
 * what it was added for - the rationale, and the measurement behind it, are in
 * settle_before_arming().  Set it to a non-zero value only to experiment.
 */
static unsigned int wait_ms;
module_param(wait_ms, uint, 0444);
MODULE_PARM_DESC(wait_ms, "At poweroff, wait up to this many ms for the display device to settle before runtime PM is disabled (0 = do not wait, the default)");

/*
 * Diagnostic knob, 0 by default.  The FINAL lines are the last thing printed
 * before the firmware is asked to power off, so without a hold they are on
 * screen only for as long as the ACPI S5 transition takes - long enough to miss
 * them.  This keeps them there for a photograph and changes nothing else; it
 * costs exactly this much extra shutdown time.
 */
static unsigned int final_hold_ms;
module_param(final_hold_ms, uint, 0444);
MODULE_PARM_DESC(final_hold_ms, "After printing the FINAL lines, wait this many ms before the poweroff continues (0 = no hold; diagnostic only)");

/* Drivers this module refuses to touch, no matter what the device list says. */
static const char *const protected_drivers[] = {
	"amdgpu", "i915", "xe", "nouveau",	/* never the integrated GPU */
	"nvme", "ahci", "sdhci", "sdhci_pci",	/* never storage */
	"xhci_hcd", "thunderbolt",		/* never USB / USB4 */
	"r8169", "mt7921e", "iwlwifi",		/* never networking */
};

/*
 * Drivers whose .shutdown is never nulled, whatever noshut= says: pcieport
 * governs every PCIe port in the machine and its callback removes the PME/AER/
 * hotplug port services (drivers/pci/pcie/portdrv.c:739-748, 791-797).
 */
static const char *const never_null[] = { "pcieport" };

static struct pci_dev *targets[S5_MAX_DEVS];
static struct pci_dev *display_target;	/* the NVIDIA display device, if one is listed */
static int n_targets;
static bool shield_armed;
static struct notifier_block s5_nb;
static struct sys_off_handler *s5_final_handler;	/* diagnostic only, may be NULL */

/*
 * THE ONE PLACE THE CLASS CODE IS DECODED.
 *
 * pdev->class is the full 24-bit class code.  include/linux/pci.h declares it as
 *
 *	unsigned int class;		// 3 bytes: (base,sub,prog-if)
 *
 * while the PCI_CLASS_* macros in pci_ids.h are 16-bit (PCI_CLASS_DISPLAY_VGA
 * is 0x0300, not 0x030000), so the shift must be 8.  The kernel's own idiom is
 * include/linux/pci.h:790:  if ((pdev->class >> 8) == PCI_CLASS_DISPLAY_VGA).
 *
 * Shifting by 16 instead yields 0x3/0x4/0x6, matches none of those macros, and
 * makes this module refuse to load its own device list - which is what revision
 * 1.0 did.  bin/s5-shield-dryrun parses this function, insists on seeing
 * "class >> 8" here, and applies the same shift to the live hardware.
 */
static unsigned int pci_class16(struct pci_dev *pdev)
{
	return pdev->class >> 8;
}

static const char *pwr_name(pci_power_t s)
{
	switch (s) {
	case PCI_D0:		return "D0";
	case PCI_D1:		return "D1";
	case PCI_D2:		return "D2";
	case PCI_D3hot:		return "D3hot";
	case PCI_D3cold:	return "D3cold";
	default:		return "unknown";
	}
}

static const char *rpm_name(enum rpm_status s)
{
	switch (s) {
	case RPM_ACTIVE:	return "active";
	case RPM_SUSPENDED:	return "suspended";
	case RPM_RESUMING:	return "resuming";
	case RPM_SUSPENDING:	return "suspending";
	case RPM_BLOCKED:	return "blocked";
	default:		return "invalid";
	}
}

/*
 * WHY the device is not asleep, in the runtime PM core's own vocabulary.
 *
 * rpm_check_suspend_allowed() (v7.2.8 drivers/base/power/runtime.c) refuses a
 * suspend for exactly these reasons, in this order, and each one has a
 * different remedy - which is why the module says which one it is instead of
 * only "still in D0":
 *
 *	-EACCES  disable_depth > 0          runtime PM is already off
 *	-EAGAIN  usage_count > 0            a driver or client holds a reference
 *	-EBUSY   child_count > 0            a device below it is still active
 *	-EPERM   resume_latency == 0        PM QoS forbids suspending
 *
 * The -EPERM case is the one this module cannot see: __dev_pm_qos_resume_latency()
 * is not exported to modules.  It is also the one that has never been observed
 * on this machine, so no blocker is reported rather than a wrong one.
 */
static const char *rpm_blocker(struct device *dev)
{
	if (dev->power.disable_depth)
		return "runtime PM is disabled for it (-EACCES)";
	if (atomic_read(&dev->power.usage_count))
		return "usage_count > 0: a driver or client still holds a reference (-EAGAIN)";
	if (!dev->power.ignore_children && atomic_read(&dev->power.child_count))
		return "active child below it (-EBUSY)";
	if (dev->power.runtime_status != RPM_ACTIVE)
		return "it is not active, so there is nothing left to suspend";
	return "none in the runtime PM accounting: an autosuspend is pending, or the driver never asked for one";
}

/* One compact, greppable line of runtime PM accounting. */
static void rpm_accounting(struct device *dev, char *buf, size_t len)
{
	struct device *parent = dev->parent;

	snprintf(buf, len, "rpm=%s use=%d child=%d dis=%u auto=%s%s%s",
		 rpm_name(dev->power.runtime_status),
		 atomic_read(&dev->power.usage_count),
		 atomic_read(&dev->power.child_count),
		 (unsigned int)dev->power.disable_depth,
		 dev->power.runtime_auto ? "yes" : "no",
		 parent ? " parent=" : "",
		 parent ? rpm_name(parent->power.runtime_status) : "");
}

#if IS_ENABLED(CONFIG_ACPI)
static const char *acpi_pwr_name(int s)
{
	switch (s) {
	case ACPI_STATE_D0:		return "D0";
	case ACPI_STATE_D1:		return "D1";
	case ACPI_STATE_D2:		return "D2";
	case ACPI_STATE_D3_HOT:		return "D3hot";
	case ACPI_STATE_D3_COLD:	return "D3cold";
	default:			return "?";
	}
}

/*
 * The ACPI node is the platform-side half of "is the rail off": on this machine
 * the slot power resource (PG00 / LNXPOWER:04) hangs off the root port's ACPI
 * device, so its power state is what decides whether the dGPU is really
 * unpowered.
 */
static const char *acpi_state(struct pci_dev *pdev)
{
	struct acpi_device *adev = ACPI_COMPANION(&pdev->dev);

	return adev ? acpi_pwr_name(adev->power.state) : "none";
}
#else
static const char *acpi_state(struct pci_dev *pdev) { return "n/a"; }
#endif

/* trim leading and trailing blanks in place, return the start */
static char *trim(char *s)
{
	char *e;

	while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
		s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
		*--e = '\0';
	return s;
}

static bool is_blank(char c)
{
	return c == '\0' || c == ',' || c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* comma separated, whitespace tolerant, exact-token match */
static bool in_list(const char *list, const char *name)
{
	const char *p = list;
	size_t n;

	if (!list || !name || !*name)
		return false;
	n = strlen(name);

	while (p && *p) {
		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
			p++;
		if (!strncmp(p, name, n) && is_blank(p[n]))
			return true;
		p = strchr(p, ',');
		if (p)
			p++;
	}
	return false;
}

static bool in_table(const char *const *table, size_t n, const char *name)
{
	size_t i;

	if (!name)
		return false;
	for (i = 0; i < n; i++)
		if (!strcmp(name, table[i]))
			return true;
	return false;
}

static bool is_protected(const char *name)
{
	return in_table(protected_drivers, ARRAY_SIZE(protected_drivers), name);
}

static bool in_never_null(const char *name)
{
	return in_table(never_null, ARRAY_SIZE(never_null), name);
}

/* copy the bound driver's name while holding the device lock */
static bool driver_name_of(struct pci_dev *pdev, char *buf, size_t len)
{
	bool have = false;

	device_lock(&pdev->dev);
	if (pdev->driver) {
		strscpy(buf, pdev->driver->name, len);
		have = true;
	}
	device_unlock(&pdev->dev);
	return have;
}

/*
 * Only display, multimedia-audio and PCI bridges may ever be listed.
 * PCI_CLASS_MULTIMEDIA_AUDIO is 0x0401 while this GPU's audio function is
 * PCI_CLASS_MULTIMEDIA_HD_AUDIO = 0x0403; both are accepted.
 */
static bool class_allowed(struct pci_dev *pdev)
{
	switch (pci_class16(pdev)) {
	case PCI_CLASS_DISPLAY_VGA:
	case PCI_CLASS_DISPLAY_3D:
	case PCI_CLASS_DISPLAY_OTHER:
	case PCI_CLASS_MULTIMEDIA_AUDIO:
	case PCI_CLASS_MULTIMEDIA_HD_AUDIO:
	case PCI_CLASS_BRIDGE_PCI:
		return true;
	default:
		return false;
	}
}

static bool is_display(struct pci_dev *pdev)
{
	switch (pci_class16(pdev)) {
	case PCI_CLASS_DISPLAY_VGA:
	case PCI_CLASS_DISPLAY_3D:
	case PCI_CLASS_DISPLAY_OTHER:
		return true;
	default:
		return false;
	}
}

/* is @child somewhere below @bridge in the PCI topology? */
static bool is_below(struct pci_dev *child, struct pci_dev *bridge)
{
	struct pci_bus *bus = child->bus;

	while (bus) {
		if (bus->self == bridge)
			return true;
		bus = bus->parent;
	}
	return false;
}

/*
 * THE STEP THAT DOES NOT PAY FOR ITSELF - AND IS THEREFORE OFF BY DEFAULT.
 *
 * Revision 1.3 waited here for D3cold, nudging the runtime PM core with
 * pm_request_idle() while it waited.  The code is kept, the default is 0, and
 * the two reasons are worth writing down because both were measured or read out
 * of the source after that revision shipped:
 *
 *  1. pm_request_idle() can only *ask*, and rpm_check_suspend_allowed()
 *     refuses with -EACCES (runtime PM disabled), -EAGAIN (usage_count > 0) or
 *     -EBUSY (a child is still active - the dGPU's own case for its root port,
 *     which is why the port cannot sleep while the GPU is awake).  A device
 *     that *can* suspend has already been idle-notified by the core when its
 *     driver dropped the last reference, so the nudge is a no-op there as well.
 *     And when it is accepted, rpm_suspend(RPM_AUTO) waits out the device's
 *     autosuspend delay instead of suspending on the spot.  There is no
 *     "near-instant" version of this to be had from inside the kernel.
 *  2. The state that decides the *rail* is reached later anyway: the drivers'
 *     own .shutdown callbacks run inside device_shutdown(), after this
 *     notifier, and that is where the references that keep the dGPU awake are
 *     finally released.  On this machine the wait ran its full 5000 ms with the
 *     GPU still in D0 (2026-10-03 15:13) - and the measured S5 that followed
 *     still cost ~1 W over 50 minutes.  The observer at the end of this file
 *     now records that final state instead of leaving it to inference.
 *
 * If wait_ms is set anyway (experiment, or a machine that behaves differently):
 * phase 1 asks every listed device to go idle exactly once, and only phase 2
 * spends the budget polling.  The single loop of revision 1.3 spent a shared
 * deadline in list order, so with the devs=bridge,gpu,audio list that
 * s5-descubre-dgpu generates the bridge ate the whole budget - a port cannot
 * suspend while the GPU below it is awake - and the GPU never got a nudge at
 * all.  Nothing here imitates that bug.
 */
static bool settled(void)
{
	int i;

	if (!display_target || display_target->current_state != PCI_D3cold)
		return false;

	/* The rail (PG00 / LNXPOWER:04) is released by the port reaching D3,
	 * so a GPU in D3cold under a bridge still in D0 is not settled. */
	for (i = 0; i < n_targets; i++)
		if (targets[i] && pci_class16(targets[i]) == PCI_CLASS_BRIDGE_PCI &&
		    targets[i]->current_state == PCI_D0)
			return false;

	return true;
}

static void settle_before_arming(void)
{
	char acc[96];
	pci_power_t st;
	unsigned int waited = 0;
	int i;

	if (!display_target)
		return;

	st = display_target->current_state;

	if (!wait_ms) {
		rpm_accounting(&display_target->dev, acc, sizeof(acc));
		if (settled())
			pci_emerg(display_target, "s5-shield: %s is already in D3cold and its bridge is out of D0 (rail off); wait_ms=0, no wait\n",
				  pci_name(display_target));
		else
			pci_emerg(display_target, "s5-shield: wait_ms=0, not waiting: %s is %s, %s [%s]. Nothing here resumes it from now on; whether this poweroff is expensive is decided inside device_shutdown() and printed by the FINAL line\n",
				  pci_name(display_target), pwr_name(st),
				  rpm_blocker(&display_target->dev), acc);
		return;
	}

	pci_emerg(display_target, "s5-shield: %s is in %s, not asleep; asking runtime PM to let go and waiting up to %u ms for D3cold with the bridge out of D0\n",
		  pci_name(display_target), pwr_name(st), wait_ms);

	/* phase 1: one idle request per listed device, independent of order */
	for (i = 0; i < n_targets; i++)
		if (targets[i])
			pm_request_idle(&targets[i]->dev);

	/* phase 2: poll, do not spend the budget nudging */
	while (waited < wait_ms) {
		msleep(S5_POLL_MS);
		waited += S5_POLL_MS;

		if (settled()) {
			pci_emerg(display_target, "s5-shield: settled after %u ms: %s %s, bridge out of D0\n",
				  waited, pci_name(display_target),
				  pwr_name(display_target->current_state));
			return;
		}
		if (display_target->current_state != st) {
			st = display_target->current_state;
			pci_emerg(display_target, "s5-shield: %s is now %s (%u ms)\n",
				  pci_name(display_target), pwr_name(st), waited);
		}
	}

	rpm_accounting(&display_target->dev, acc, sizeof(acc));
	pci_emerg(display_target, "s5-shield: not settled after %u ms: %s still %s, %s [%s]; arming anyway (see README: Reading the console)\n",
		  waited, pci_name(display_target),
		  pwr_name(display_target->current_state),
		  rpm_blocker(&display_target->dev), acc);
}

static int s5_shield_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	int i, disabled = 0, nulled = 0, skipped = 0;

	if (action != SYS_POWER_OFF)
		return NOTIFY_DONE;

	pr_emerg("s5-shield: poweroff path, %d device(s) listed\n", n_targets);

	/* Nothing below can help if the device is awake: let it fall asleep
	 * first (see settle_before_arming()). */
	settle_before_arming();

	for (i = 0; i < n_targets; i++) {
		struct pci_dev *pdev = targets[i];
		struct pci_driver *drv;
		const char *dn;
		char acc[96];
		bool do_disable = false, do_null = false, bridge;

		if (!pdev)
			continue;

		bridge = (pci_class16(pdev) == PCI_CLASS_BRIDGE_PCI);

		if (!class_allowed(pdev)) {
			pci_emerg(pdev, "s5-shield: class %#x (decoded %#06x) no longer allowed, skipped\n",
				  pdev->class, pci_class16(pdev));
			skipped++;
			continue;
		}

		/* device_lock() serialises us against a concurrent driver
		 * unbind/rmmod, exactly as the shutdown walk does later. */
		device_lock(&pdev->dev);
		drv = pdev->driver;
		dn = drv ? drv->name : NULL;
		rpm_accounting(&pdev->dev, acc, sizeof(acc));

		pci_emerg(pdev, "s5-shield: state=%s acpi=%s driver=%s %s\n",
			  pwr_name(pdev->current_state), acpi_state(pdev),
			  dn ? dn : "(no driver)", acc);

		if (is_protected(dn)) {
			pci_emerg(pdev, "s5-shield: SKIPPED, driver '%s' is protected\n", dn);
			skipped++;
			goto next;
		}

		if (!drv || !drv->shutdown) {
			/* no callback exists, so (a) alone cannot leave one
			 * running against a sleeping device */
			do_disable = true;
		} else if (!in_never_null(dn) && in_list(noshut, dn)) {
			do_disable = true;
			do_null = true;
		} else if (bridge) {
			/* deliberate exception: disable the port's runtime PM,
			 * but leave pcieport's .shutdown intact (it governs
			 * every port on the machine) */
			pci_emerg(pdev, "s5-shield: bridge: .shutdown of '%s' left in place on purpose\n", dn);
			do_disable = true;
		} else {
			/* THE ONE COMBINATION THAT BUS-LOCKS THIS HARDWARE.
			 * Refuse to do half the job. */
			pci_emerg(pdev, "s5-shield: SKIPPED: driver '%s' has a .shutdown callback not covered by noshut=; not disabling runtime PM without nulling it\n", dn);
			skipped++;
			goto next;
		}

		if (do_disable) {
			__pm_runtime_disable(&pdev->dev, false);
			disabled++;
			if (pdev->current_state == PCI_D0)
				pci_emerg(pdev, "s5-shield: still in D0, cannot suspend after this: this poweroff can still be warm (README: Reading the console)\n");
		}
		if (do_null) {
			drv->shutdown = NULL;
			nulled++;
			pci_emerg(pdev, "s5-shield: .shutdown of '%s' nulled\n", dn);
		}
next:
		device_unlock(&pdev->dev);
	}

	if (disabled || nulled)
		shield_armed = true;

	pr_emerg("s5-shield: done: runtime PM off on %d, .shutdown nulled on %d, skipped %d\n",
		 disabled, nulled, skipped);

	return NOTIFY_DONE;
}

/*
 * THE STATE AT THE MOMENT OF NO RETURN.
 *
 * Everything above runs *before* device_shutdown().  The state that decides
 * whether the dGPU rail is cut is the one the tree is left in *after* that
 * walk, and until revision 1.4 nothing here could see it - which is why the
 * README could only say "the GPU was in D0 when the shield armed, and the S5
 * was still ~1 W" without being able to say why.
 *
 * v7.2.8 kernel_power_off() is:
 *
 *	kernel_shutdown_prepare(SYSTEM_POWER_OFF)   [reboot notifier, then
 *						     device_shutdown()]
 *	do_kernel_power_off_prepare()               [this observer]
 *	migrate_to_reboot_cpu()
 *	syscore_shutdown()
 *	machine_power_off()                         [firmware, ACPI _PTS(5)]
 *
 * so a SYS_OFF_MODE_POWER_OFF_PREPARE handler runs after every driver's
 * .shutdown callback and before the firmware is asked to power off: the last
 * observable moment.  This one only reads and prints, and returns NOTIFY_DONE
 * so the chain continues to whatever actually powers the machine off.  It
 * cannot hang a poweroff: handlers in this mode are allowed to sleep, and the
 * only locks taken are the device locks the shutdown walk itself takes.
 *
 * How to read the result (also in README: Reading the console):
 *	dGPU D3cold + bridge D3hot/D3cold  -> the rail was released before the
 *	    firmware took over, i.e. the shield worked by *not resuming* it, and
 *	    the D0 seen at arming time was not the deciding factor;
 *	a device still in D0 -> whatever held it awake (see use=/child= in the
 *	    accounting) was released too late or never, and that is the case that
 *	    needs the shutdown-time policy/fallback, not a longer wait.
 */
static int s5_shield_observe_final(struct sys_off_data *data)
{
	int i;

	(void)data;

	pr_emerg("s5-shield: FINAL state, after device_shutdown() and before the firmware poweroff\n");

	for (i = 0; i < n_targets; i++) {
		struct pci_dev *pdev = targets[i];
		struct pci_driver *drv;
		const char *dn;
		char acc[96];

		if (!pdev)
			continue;

		device_lock(&pdev->dev);
		drv = pdev->driver;
		dn = drv ? drv->name : NULL;
		rpm_accounting(&pdev->dev, acc, sizeof(acc));
		pci_emerg(pdev, "s5-shield: FINAL %s pci=%s acpi=%s driver=%s %s\n",
			  pci_name(pdev), pwr_name(pdev->current_state),
			  acpi_state(pdev), dn ? dn : "(no driver)", acc);
		device_unlock(&pdev->dev);
	}

	if (final_hold_ms)
		msleep(final_hold_ms);

	return NOTIFY_DONE;
}
/* report noshut= entries that match no driver bound to a listed device */
static void check_noshut_coverage(void)
{
	char *copy, *rest, *tok;

	if (!noshut || !*noshut)
		return;

	copy = kstrdup(noshut, GFP_KERNEL);
	if (!copy)
		return;
	rest = copy;

	while ((tok = strsep(&rest, ",")) != NULL) {
		char dname[S5_NAME_LEN];
		int i;
		bool found = false;

		tok = trim(tok);
		if (!*tok)
			continue;

		for (i = 0; i < n_targets; i++) {
			if (driver_name_of(targets[i], dname, sizeof(dname)) &&
			    !strcmp(dname, tok)) {
				found = true;
				break;
			}
		}
		if (!found)
			pr_warn("s5-shield: noshut= names '%s', but no listed device is bound to such a driver right now; if that is still true at poweroff the affected device will be skipped rather than half-shielded\n",
				tok);
	}
	kfree(copy);
}

static int __init s5_shield_init(void)
{
	char *copy, *rest, *tok;
	int nvidia_display = 0, i, j;

	if (!devs || !*devs) {
		pr_err("s5-shield: devs= is empty, refusing to load (nothing to shield)\n");
		return -EINVAL;
	}

	copy = kstrdup(devs, GFP_KERNEL);
	if (!copy)
		return -ENOMEM;
	rest = copy;

	while ((tok = strsep(&rest, ",")) != NULL) {
		unsigned int dom, bus, slot, fn;
		int consumed = 0;
		struct pci_dev *pdev;
		char dname[S5_NAME_LEN];

		tok = trim(tok);
		if (!*tok)
			continue;

		if (n_targets >= S5_MAX_DEVS) {
			pr_err("s5-shield: too many devices (max %d)\n", S5_MAX_DEVS);
			goto reject;
		}
		if (sscanf(tok, "%4x:%2x:%2x.%1x%n", &dom, &bus, &slot, &fn, &consumed) != 4 ||
		    tok[consumed] != '\0' || bus > 0xff || slot > 0x1f || fn > 0x7) {
			pr_err("s5-shield: cannot parse BDF '%s'\n", tok);
			goto reject;
		}

		pdev = pci_get_domain_bus_and_slot(dom, bus, PCI_DEVFN(slot, fn));
		if (!pdev) {
			pr_err("s5-shield: no such device %s\n", tok);
			goto reject;
		}
		if (!class_allowed(pdev)) {
			pr_err("s5-shield: %s class %#x (decoded pci_class16() = %#06x) is not display/audio/bridge, refusing. If that looks wrong, the class shift in pci_class16() is the first thing to check - run bin/s5-shield-dryrun\n",
			       tok, pdev->class, pci_class16(pdev));
			pci_dev_put(pdev);
			goto reject;
		}
		if (driver_name_of(pdev, dname, sizeof(dname)) && is_protected(dname)) {
			pr_err("s5-shield: %s is bound to protected driver '%s', refusing\n",
			       tok, dname);
			pci_dev_put(pdev);
			goto reject;
		}

		if (pdev->vendor == PCI_VENDOR_ID_NVIDIA && is_display(pdev)) {
			nvidia_display++;
			if (!display_target)
				display_target = pdev;
		}

		/* reference kept for the lifetime of the module */
		targets[n_targets++] = pdev;
	}
	kfree(copy);

	/*
	 * A listed bridge must really be an ancestor of a listed non-bridge
	 * device: that is what makes it "the root port the dGPU hangs off"
	 * rather than some unrelated port whose runtime PM we would be
	 * disabling for no reason.
	 */
	for (i = 0; i < n_targets; i++) {
		bool found = false;

		if (pci_class16(targets[i]) != PCI_CLASS_BRIDGE_PCI)
			continue;

		for (j = 0; j < n_targets && !found; j++) {
			if (i == j)
				continue;
			if (pci_class16(targets[j]) == PCI_CLASS_BRIDGE_PCI)
				continue;
			found = is_below(targets[j], targets[i]);
		}
		if (!found) {
			pr_err("s5-shield: %s is a bridge but nothing listed sits below it, refusing\n",
			       pci_name(targets[i]));
			goto reject_nocopy;
		}
	}

	if (!nvidia_display) {
		pr_err("s5-shield: no NVIDIA display device in the list, refusing\n");
		goto reject_nocopy;
	}

	s5_nb.notifier_call = s5_shield_notify;
	s5_nb.priority = 0;
	if (register_reboot_notifier(&s5_nb)) {
		pr_err("s5-shield: cannot register reboot notifier\n");
		goto reject_nocopy;
	}

	/*
	 * Diagnostic only: if this cannot be registered the shield still works,
	 * we just lose the FINAL line.  Registered here rather than at arming
	 * time because a handler registered from inside the reboot notifier
	 * would be racing the device walk for no reason.
	 */
	s5_final_handler = register_sys_off_handler(SYS_OFF_MODE_POWER_OFF_PREPARE,
						    SYS_OFF_PRIO_DEFAULT,
						    s5_shield_observe_final, NULL);
	if (IS_ERR(s5_final_handler)) {
		pr_warn("s5-shield: cannot register the FINAL observer (%ld); the shield itself is unaffected\n",
			PTR_ERR(s5_final_handler));
		s5_final_handler = NULL;
	}

	check_noshut_coverage();

	pr_info("s5-shield: ready: %d target(s), noshut=%s, wait_ms=%u, FINAL observer=%s. No effect until poweroff.\n",
		n_targets, noshut, wait_ms, s5_final_handler ? "on" : "off");
	return 0;

reject:
	kfree(copy);
reject_nocopy:
	while (n_targets > 0)
		pci_dev_put(targets[--n_targets]);
	display_target = NULL;
	return -EINVAL;
}

static void __exit s5_shield_exit(void)
{
	if (s5_final_handler) {
		unregister_sys_off_handler(s5_final_handler);
		s5_final_handler = NULL;
	}
	unregister_reboot_notifier(&s5_nb);
	while (n_targets > 0)
		pci_dev_put(targets[--n_targets]);
	display_target = NULL;

	if (shield_armed)
		pr_warn("s5-shield: unloaded, but the shield was already armed during this boot; only a reboot clears it\n");
	else
		pr_info("s5-shield: unloaded, nothing was armed\n");
}

module_init(s5_shield_init);
module_exit(s5_shield_exit);

MODULE_LICENSE("GPL");
MODULE_VERSION("1.4");
MODULE_DESCRIPTION("Keep the discrete GPU in D3cold during poweroff (S5 rail-off fix for HP OMEN 16-ap0xxx)");
MODULE_AUTHOR("prepared for december172");
