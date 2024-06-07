//
// Created by kfeng on 12/24/21.
//

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/hardirq.h>
#include <linux/preempt.h>
#include <linux/sched.h>

MODULE_LICENSE("Dual BSD/GPL");

#define SIZE_OF_STAT 100000
#define BOUND_OF_LOOP 1000
#define UINT64_MAX (18446744073709551615ULL)

void inline
Filltimes(uint64_t
**times) {
unsigned long flags;
int i, j;
uint64_t start, end;
unsigned cycles_low, cycles_high, cycles_low1, cycles_high1;
volatile int variable = 0;

asm volatile ("CPUID\n\t"
              "RDTSC\n\t"
              "mov %%edx, %0\n\t"
              "mov %%eax, %1\n\t": "=r" (cycles_high), "=r" (cycles_low)::"%rax", "%rbx", "%rcx", "%rdx");
asm volatile ("RDTSCP\n\t"
              "mov %%edx, %0\n\t"
              "mov %%eax, %1\n\t"
              "CPUID\n\t": "=r" (cycles_high1), "=r" (cycles_low1)::"%rax", "%rbx", "%rcx", "%rdx");
asm volatile ("CPUID\n\t"
              "RDTSC\n\t"
              "mov %%edx, %0\n\t"
              "mov %%eax, %1\n\t": "=r" (cycles_high), "=r" (cycles_low)::"%rax", "%rbx", "%rcx", "%rdx");
asm volatile ("RDTSCP\n\t"
              "mov %%edx, %0\n\t"
              "mov %%eax, %1\n\t"
              "CPUID\n\t": "=r" (cycles_high1), "=r" (cycles_low1)::"%rax", "%rbx", "%rcx", "%rdx");

for (
j = 0;
j < BOUND_OF_LOOP; j++) {
for (
i = 0;
i < SIZE_OF_STAT; i++) {

variable = 0;

preempt_disable();

raw_local_irq_save(flags);

asm volatile ("RDTSCP\n\t"          /* read the clock */
              "LFENCE\n\t"          /* serialize */
              "mov %%edx, %0\n\t"
              "mov %%eax, %1\n\t": "=r" (cycles_high), "=r" (cycles_low)::"%rax", "%rbx", "%rcx", "%rdx");
/*call the function to measure here*/
asm volatile ("RDTSCP\n\t"          /* read the clock */
              "LFENCE\n\t"          /* serialize */
              "mov %%edx, %0\n\t"
              "mov %%eax, %1\n\t": "=r" (cycles_high1), "=r" (cycles_low1)::"%rax", "%rbx", "%rcx", "%rdx");

raw_local_irq_restore(flags);

preempt_enable();

start = (((uint64_t)
cycles_high << 32) | cycles_low);

end = (((uint64_t)
cycles_high1 << 32) | cycles_low1);

if ((end - start) < 0) {
printk(KERN_ERR
"\n\n>>>>>>>>>>>>>>CRITICAL ERROR IN TAKING THE TIME!!!!!!\n"
"loop( % d) stat( % d) start = %llu, end = %llu, variable = %u\n",
j, i, start, end, variable);
times[j][i] = 0;
} else {
times[j][i] = end -
start;
}}}
return;
}

uint64_t var_calc(uint64_t*inputs, int size)
{
    int i;
    uint64_t acc = 0, previous = 0, temp_var = 0;
    for(i = 0; i < size; i++)
    {
        if(acc < previous) goto overflow;
        previous = acc;
        acc += inputs[i];
    }
    acc = acc * acc;
    if(acc < previous) goto overflow;
    previous = 0;
    for(i = 0; i < size; i++)
    {
        if(temp_var < previous) goto overflow;
        previous = temp_var;
        temp_var += (inputs[i] * inputs[i]);
    }
    temp_var = temp_var * size;
    if(temp_var < previous) goto overflow;
    temp_var = (temp_var - acc) / (((uint64_t)(size)) * ((uint64_t)(size)));
    return (temp_var);
    overflow:
    printk(KERN_ERR
    "\n\n>>>>>>>>>>>>>>CRITICAL OVERFLOW ERROR IN var_calc!!!!!!\n\n");
    return -EINVAL;
}

uint64_t mean_calc(uint64_t*inputs, int size)
{
    int i;
    uint64_t acc = 0, previous = 0, temp_var = 0;
    for(i = 0; i < size; i++)
    {
        if(acc < previous) goto overflow;
        previous = acc;
        acc += inputs[i];
    }
    temp_var = acc / size;
    return (temp_var);
    overflow:
    printk(KERN_ERR
    "\n\n>>>>>>>>>>>>>>CRITICAL OVERFLOW ERROR IN mean_calc!!!!!!\n\n");
    return -EINVAL;
}

uint64_t dev_sq_calc(uint64_t*inputs, int size)
{
    int i;
    uint64_t diff_sq_acc = 0, previous = 0;
    for(i = 0; i < size; i++)
    {
        if(diff_sq_acc < previous) goto overflow;
        previous = diff_sq_acc;
        diff_sq_acc += (inputs[i] * inputs[i]); /* assume inputs is an array of absolute diff, not raw values */
    }
    diff_sq_acc = diff_sq_acc / size;
    return (diff_sq_acc);
    overflow:
    printk(KERN_ERR
    "\n\n>>>>>>>>>>>>>>CRITICAL OVERFLOW ERROR IN stdev_calc!!!!!!\n\n");
    return -EINVAL;
}

static int __init

kernel_clock_test_start(void)
{
    int i = 0, j = 0, spurious = 0, k = 0;
    uint64_t * *times;
    uint64_t * variances;
    uint64_t * min_values;
    uint64_t max_dev = 0, min_time = 0, max_time = 0, prev_min = 0, tot_var = 0, max_dev_all = 0, var_of_vars = 0, var_of_mins = 0;
    uint64_t overall_min = 0, overall_max = 0, overall_mean = 0, local_mean_acc = 0;
    uint64_t overall_dev_sq = 0, local_dev_sq = 0;

    printk(KERN_INFO
    "Loading kernel_clock_test module...\n");
    times = kmalloc(BOUND_OF_LOOP * sizeof(uint64_t * ), GFP_KERNEL);
    if(!times)
    {
        printk(KERN_ERR
        "unable to allocate memory for times\n");
        return 0;
    }

    for(j = 0; j < BOUND_OF_LOOP; j++)
    {
        times[j] = kmalloc(SIZE_OF_STAT * sizeof(uint64_t), GFP_KERNEL);
        if(!times[j])
        {
            printk(KERN_ERR
            "unable to allocate memory for times[%d]\n", j);
            for(k = 0; k < j; k++)
                kfree(times[k]);
            return 0;
        }
    }

    variances = kmalloc(BOUND_OF_LOOP * sizeof(uint64_t), GFP_KERNEL);
    if(!variances)
    {
        printk(KERN_ERR
        "unable to allocate memory for variances\n");
        return 0;
    }

    min_values = kmalloc(BOUND_OF_LOOP * sizeof(uint64_t), GFP_KERNEL);
    if(!min_values)
    {
        printk(KERN_ERR
        "unable to allocate memory for min_values\n");
        return 0;
    }

    Filltimes(times);

    for(j = 0; j < BOUND_OF_LOOP; j++)
    {

        max_dev = 0;
        min_time = 0;
        max_time = 0;

        for(i = 0; i < SIZE_OF_STAT; i++)
        {
            if((min_time == 0) || (min_time > times[j][i]))
                min_time = times[j][i];
            if(max_time < times[j][i])
                max_time = times[j][i];
            if((overall_min == 0) || (overall_min > times[j][i]))
                overall_min = times[j][i];
            if(overall_max < times[j][i])
                overall_max = times[j][i];
        }

        max_dev = max_time - min_time;
        min_values[j] = min_time;

        if((prev_min != 0) && (prev_min > min_time))
            spurious++;
        if(max_dev > max_dev_all)
            max_dev_all = max_dev;

        variances[j] = var_calc(times[j], SIZE_OF_STAT);
        tot_var += variances[j];

        printk(KERN_ERR
        "loop_size:%d >>>> variance(cycles): %llu; max_deviation: %llu; min time: %llu", j, variances[j], max_dev, min_time);
        prev_min = min_time;
    }

    var_of_vars = var_calc(variances, BOUND_OF_LOOP);
    var_of_mins = var_calc(min_values, BOUND_OF_LOOP);

    printk(KERN_ERR
    "\n total number of spurious min values = %d", spurious);
    printk(KERN_ERR
    "\n total variance = %llu", (tot_var / BOUND_OF_LOOP));
    printk(KERN_ERR
    "\n absolute max deviation = %llu", max_dev_all);
    printk(KERN_ERR
    "\n variance of variances = %llu", var_of_vars);
    printk(KERN_ERR
    "\n variance of minimum values = %llu", var_of_mins);

    for(j = 0; j < BOUND_OF_LOOP; j++)
    {
        local_mean_acc += mean_calc(times[j], SIZE_OF_STAT);
    }
    overall_mean = local_mean_acc / BOUND_OF_LOOP;
    for(j = 0; j < BOUND_OF_LOOP; j++)
    {
        for(i = 0; i < SIZE_OF_STAT; i++)
        {
            times[j][i] = abs(times[j][i] - overall_mean);
        }
        local_dev_sq = dev_sq_calc(times[j], SIZE_OF_STAT);
        overall_dev_sq += local_dev_sq;
        printk(KERN_ERR
        "loop_size:%d >>>> dev square(cycles): %llu", j, local_dev_sq);
    }
    overall_dev_sq = overall_dev_sq / BOUND_OF_LOOP;

    printk(KERN_ERR
    "\n overall minimum = %llu", overall_min);
    printk(KERN_ERR
    "\n overall maximum = %llu", overall_max);
    printk(KERN_ERR
    "\n overall mean = %llu", overall_mean);
    printk(KERN_ERR
    "\n overall stdev = %llu", overall_dev_sq);

    for(j = 0; j < BOUND_OF_LOOP; j++)
    {
        kfree(times[j]);
    }
    kfree(times);
    kfree(variances);
    kfree(min_values);
    return 0;
}

static void __exit

kernel_clock_test_end(void)
{
    printk(KERN_INFO
    "Goodbye Mr.\n");
}

module_init(kernel_clock_test_start);
module_exit(kernel_clock_test_end);