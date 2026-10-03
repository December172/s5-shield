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
 *	(0) the display device has been given the chance to fall asleep, and the
 *	    module has waited (wait_ms=, default 5 s) until it reports D3cold -
 *	    the state that means the rail is actually off.  Without this step
 *	    (a) below can pin an awake device awake, see settle_before_arming();
 *	(a) runtime PM is disabled on the listed devices, so the
 *	    pm_runtime_resume() above bounces with -EACCES instead of powering
 *	    the GPU up (v7.2.8: drivers/base/power/runtime.c:796-808); and
 *	(b) drv->shutdown is NULL for the drivers named in `noshut`, so
 *	    nv_pci_shutdown() (and azx_shutdown(), which does MMIO through
 *	    remap_addr with no power-state check - sound/hda/controllers/intel.c
 *	    :2465-2474) cannot run against a device that is in D3cold.
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
 *	     silently useless in exactly the case it exists for.  The wait is
 *	     also the only thing that can make the *rail* drop (a port cannot
 *	     sleep while a child is awake, and the slot power resource that
 *	     feeds the dGPU is released by the port's D3), and it nudges the
 *	     runtime PM core with pm_request_idle() while it waits, which is
 *	     the kernel-side lever README section 7 has always pointed at.
 *
 *	     bin/s5-shield-dryrun now reads the shift out of pci_class16() and the
 *	     accept-list out of class_allowed(), resolves both against the
 *	     running kernel's pci_ids.h and the live hardware, and refuses to let
 *	     install.sh proceed if they disagree.  That guard reproduces both
 *	     1.0/1.1 bugs; a mirror that cannot disagree with the source is
 *	     worthless, which is exactly how 1.0 shipped.
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
 * How long the notifier waits for the listed display device to actually reach
 * D3cold before it disables runtime PM.  See settle_before_arming().  0 turns
 * the wait off, i.e. restores the revision 1.2 behaviour of only preventing a
 * resume.
 */
static unsigned int wait_ms = 5000;
module_param(wait_ms, uint, 0444);
MODULE_PARM_DESC(wait_ms, "At poweroff, wait up to this many ms for the listed display device to reach D3cold before runtime PM is disabled (0 = do not wait)");

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
 * THE HALF REVISION 1.2 WAS MISSING.
 *
 * Everything else in this module only *keeps* the dGPU asleep: it stops
 * pci_device_shutdown() from resuming it.  But if the device is still in D0
 * when this notifier runs, __pm_runtime_disable() freezes it there - runtime
 * PM cannot suspend it any more either - so the rail stays on for the whole of
 * S5 and the fix is silently useless in exactly the case it exists for.
 *
 * Waiting is also the only way the *rail* can drop at all: the slot power
 * resource that feeds the dGPU is released through the ACPI power resource of
 * its root port, and a port cannot suspend while a child below it is awake
 * (v7.2.8 drivers/base/power/runtime.c: rpm_resume() walks the parent chain,
 * and a child in RPM_ACTIVE keeps the parent's child_count non-zero, so the
 * port never reaches D3 and PG00/LNXPOWER:04 is never turned off).
 *
 * So: nudge the runtime PM core with pm_request_idle() - the same idle
 * evaluation a driver's own pm_runtime_put() triggers, which is the
 * kernel-side lever README section 7 points at - and poll until the display
 * device reports D3cold, or until wait_ms runs out.  D3cold is the state that
 * actually means "the rail is off"; D3hot still has the device powered.
 *
 * Nothing here forces anything: if a driver still holds a reference the device
 * stays awake, we say so, and the poweroff is no worse than before.  The wait
 * is bounded, happens in process context (kernel_power_off() calls this from
 * the poweroff syscall, interrupts on, usermodehelper not yet disabled) and
 * only ever delays a poweroff that was going to happen anyway.
 */
static void settle_before_arming(void)
{
	pci_power_t st;
	unsigned int waited = 0;
	int i;

	if (!wait_ms || !display_target)
		return;

	st = display_target->current_state;
	if (st == PCI_D3cold) {
		pci_emerg(display_target, "s5-shield: %s is already in D3cold (rail off), no wait needed\n",
			  pci_name(display_target));
		return;
	}

	pci_emerg(display_target, "s5-shield: %s is in %s, not asleep; asking runtime PM to let go and waiting up to %u ms for D3cold\n",
		  pci_name(display_target), pwr_name(st), wait_ms);

	while (waited < wait_ms) {
		for (i = 0; i < n_targets; i++)
			if (targets[i])
				pm_request_idle(&targets[i]->dev);

		msleep(S5_POLL_MS);
		waited += S5_POLL_MS;

		if (display_target->current_state == PCI_D3cold) {
			pci_emerg(display_target, "s5-shield: %s reached D3cold after %u ms\n",
				  pci_name(display_target), waited);
			return;
		}
		if (display_target->current_state != st) {
			st = display_target->current_state;
			pci_emerg(display_target, "s5-shield: %s is now %s (%u ms)\n",
				  pci_name(display_target), pwr_name(st), waited);
		}
	}

	pci_emerg(display_target, "s5-shield: %s is still %s after %u ms, giving up on the wait; if this poweroff stays warm it is because the device was never asleep, not because the kernel woke it (README section 7)\n",
		  pci_name(display_target), pwr_name(display_target->current_state), waited);
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

		pci_emerg(pdev, "s5-shield: state=%s driver=%s\n",
			  pwr_name(pdev->current_state), dn ? dn : "(no driver)");

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
				pci_emerg(pdev, "s5-shield: still in D0, cannot suspend after this: this poweroff can still be warm (README section 7)\n");
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

	check_noshut_coverage();

	pr_info("s5-shield: ready: %d target(s), noshut=%s, wait_ms=%u. No effect until poweroff.\n",
		n_targets, noshut, wait_ms);
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
MODULE_VERSION("1.3");
MODULE_DESCRIPTION("Keep the discrete GPU in D3cold during poweroff (S5 rail-off fix for HP OMEN 16-ap0xxx)");
MODULE_AUTHOR("prepared for december172");
