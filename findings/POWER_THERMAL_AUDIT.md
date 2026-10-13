# Battery and heat: audit of what the kernel can actually change

The question this answers: what can be done *in the kernel itself* to improve
battery life and heat without losing performance.

## What was measured

This build's config was diffed against the stock kernel's `/proc/config.gz`
(kernel 6.6.118, 7762 lines) over the whole `power` / `thermal` / `sched` /
`devfreq` / `rcu` / `psi` / `idle` space.

**Three keys differ, and all three are additions in this tree:**

    CONFIG_MQ_IOSCHED_ADIOS=y            stock: absent
    CONFIG_MQ_IOSCHED_DEFAULT_ADIOS=y    stock: absent
    CONFIG_SCHED_BORE=y                  stock: absent

Every other power-relevant key matches stock exactly, including the ones
people usually tune for battery:

    HZ=250                       matches
    NO_HZ / NO_HZ_IDLE           matches (tickless idle already on)
    CPU_IDLE_GOV_TEO + MENU      matches
    RCU_NOCB_CPU + RCU_LAZY      matches
    RCU_LAZY_DEFAULT_OFF=y       matches
    ENERGY_MODEL                 matches
    CPU_FREQ_GOV_SCHEDUTIL       matches
    THERMAL_GOV_STEP_WISE        matches
    SCHED_AUTOGROUP              matches (off)
    WQ_POWER_EFFICIENT_DEFAULT   matches (off)
    IRQ_TIME_ACCOUNTING          matches (on)

The QCOM power drivers (`CONFIG_QTI_THERMAL_LIMITS_DCVS`,
`CONFIG_QTI_THERMAL_VENDOR_HOOK`, `CONFIG_ARM_QCOM_CPUFREQ_THERMAL`,
`CONFIG_CPU_IDLE_GOV_QCOM_LPM`, `CONFIG_MSM_RPM_SMD`,
`CONFIG_THERMAL_TSENS_LEGACY`, ...) are present here and disabled, and are
absent from the stock config entirely. That is correct and not a regression:
on SM8750 those live in the vendor modules shipped in `/vendor_dlkm`, and
building them into the kernel would conflict with the stock `.ko`.

## Conclusion

**There is no config-level win available. This tree is already at parity with
stock.** Everything that normally yields battery from a kernel config
(tickless, TEO, EAS, schedutil, lazy RCU) either already matches stock or is
not the kernel's to own.

Battery and heat on HyperOS 4 are therefore a framework, thermal-HAL and
runtime-governor matter, not a patch matter. Claiming a kernel-side heat
improvement without a measurement would be exactly the placebo this project
refuses to ship.

## Candidate experiments, in order of honesty

None of these has been run. This is a list of hypotheses, not a result.

1. **Runtime `schedutil` tuning, no rebuild.** In
   `/sys/devices/system/cpu/cpufreq/policy*/` there is `up_rate_limit_us`,
   `down_rate_limit_us`, `hispeed_freq` and `boost`. Not a kernel change, but
   the effect on heat is direct and measurable, and it can be A/B tested in a
   single Geekbench run plus a temperature reading.

2. **ADIOS as the default I/O scheduler.** This tree added it; stock does not
   have it. Compare against `mq-deadline` by idle drain: switch via
   `/sys/block/sd*/queue/scheduler` and measure. If ADIOS itself leaks current
   at idle, drop it as the default.

3. **Drop `CONFIG_RCU_LAZY_DEFAULT_OFF`** (i.e. make lazy RCU the default).
   Fewer RCU callbacks on the wake path means idle residency holds better.
   Low risk, but this *diverges from stock*, so only with a before/after idle
   drain measurement. Stock chose OFF; it may have had a reason.

4. **Thermal trip points.** These come from the DTB (here, the stock DTB in
   `vendor_boot`), not from the kernel. Changing them means changing the DTB,
   and that is heat bought with performance - the wrong trade for this project.
