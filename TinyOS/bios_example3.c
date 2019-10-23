#include <bios.h>
#include <stdio.h>

void alarm_handler()
{
	fprintf(stderr, "ALARM on core %u\n",cpu_core_id);
}

void bootfunc() {
	int alarm_time = cpu_core_id+1;
	fprintf(stderr, "Core %u setting alarm at %d sec.\n", 
		cpu_core_id, alarm_time);

	//on ALARM, execute alarm_handler();
	cpu_interrupt_handler(ALARM, alarm_handler);
	

	//After set time, raise ALARM signal.
	bios_set_timer(1000000 * alarm_time);
	
	fprintf(stderr, "Core %u halting.\n", cpu_core_id);


	//Halts and wakes up when ALARM signal arrives.
	cpu_core_halt();
	
	fprintf(stderr, "Core %u woke up\n", cpu_core_id);  
}

int main()
{
	/* Run simulation with 4 cores */
	vm_boot(bootfunc, 4, 0);
	return 0;
}

