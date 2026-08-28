#include <hardware/gpio.h>
#include <hardware/timer.h>
#include <pico/time.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "pico/stdlib.h"
#define LED_PIN 15
#define LED_MASK (1u<<LED_PIN)
#define GPIO15_CTRL (IO_BANK0_BASE+0x07Cu)
#define GPIO_OE_SET (SIO_BASE+0x024u)
#define GPIO_OUT_SET (SIO_BASE+0x014u)
#define GPIO_OUT_CLR (SIO_BASE+0x018u)
#define FUNCSEL_MASK 0x1Fu
#define COUNTER_BUDGET_US 100u
#define FAULT_LOG_SIZE 6u

volatile uint32_t *gpio_oe_set=(volatile uint32_t *) GPIO_OE_SET;
volatile uint32_t *gpio_out_set= (volatile uint32_t *) GPIO_OUT_SET;
volatile uint32_t *gpio_out_clear= (volatile uint32_t *) GPIO_OUT_CLR;

typedef struct {    
        uint32_t taskRuns;
        uint32_t overRun;
        bool overRunFault;
        uint64_t maxExec;
}taskHealth;

typedef struct{
    uint64_t start;
    uint64_t end;
}taskTime;
    

typedef enum{
    FaultNone,
    FaultOverrun,
    FaultStall,
    FaultSensorRange,
    FaultUART, 
}FaultId;

typedef enum{
    SEVERITY_INFO,
    SEVERITY_WARNING,
    SEVERITY_ERROR,
    SEVERITY_CRITICAL
}FaultSeverity;

typedef struct{
    FaultId id;
    FaultSeverity Severity;
    uint64_t timestampUs;

}FaultRecord;

typedef struct{
    FaultRecord faultLogsArr[FAULT_LOG_SIZE];
    uint32_t validCount;
    uint32_t writeIndex;
} FaultLogs;

void led_task(bool* toggleFlag)
{
    if(*toggleFlag)
        *gpio_out_set=LED_MASK;
    else
        *gpio_out_clear=LED_MASK;
    (*toggleFlag)=!(*toggleFlag);

}

void counter_task(uint32_t* counter)
{
    (*counter)++;

}

void health_task(uint32_t overRun, bool overRunFault, uint64_t maxCounterExec,uint32_t TaskRuns)
{
    printf("HEALTH CHECK: Total OverRuns: %u, overRunFault: %d, Max Counter Exec Time: %llu, health counter: %u \n", overRun,overRunFault,maxCounterExec,TaskRuns);

}


void record_faults(FaultId id, FaultSeverity severity, FaultLogs *faultLogs)
{
    FaultRecord newRecord;
    newRecord.id=id;
    newRecord.Severity=severity;
    newRecord.timestampUs=time_us_64();
    faultLogs->faultLogsArr[faultLogs->writeIndex]=newRecord;
    faultLogs->writeIndex=(faultLogs->writeIndex+1)%FAULT_LOG_SIZE;
    printf("Fault Record: ID:%u, Severity:%u, Time Stamp:%llu\n",newRecord.id,newRecord.Severity,newRecord.timestampUs);
    if(faultLogs->validCount<FAULT_LOG_SIZE)
        faultLogs->validCount++;
}

void print_fault_logs(const FaultLogs *faultLogs)
{
    uint32_t oldest=0;
    if(faultLogs->validCount==FAULT_LOG_SIZE)
        oldest=(faultLogs->writeIndex);
    
    for(int i=0;i<faultLogs->validCount;i++)
    {
        uint32_t ind=(oldest+i)%FAULT_LOG_SIZE;
        printf("Fault Record: ID:%u, Severity:%u, Time Stamp:%llu\n",faultLogs->faultLogsArr[ind].id,faultLogs->faultLogsArr[ind].Severity,faultLogs->faultLogsArr[ind].timestampUs);

    }

}


int main()
{
    stdio_init_all();
    uint32_t current=0;
    uint32_t lastToggle=0;
    bool toggleFlag=true;
    uint32_t counter=0;
    uint32_t lastCounter=0;
    uint64_t ledExec=0;
    uint64_t counterExec=0;
    uint32_t lastHealthCheck=0;
    taskHealth counterHealth={0,0,0};
    taskTime led={0,0};
    taskTime counterTime={0,0};
    FaultLogs faultLogs={0};
    // uint32_t recId=0;
    bool injectCounterOverrun = true;

    
    volatile uint32_t *reg15=(volatile uint32_t *) GPIO15_CTRL;
    uint32_t reg=*reg15;
    reg&=~FUNCSEL_MASK;
    reg|=5u; 
    *reg15=reg;
    *gpio_oe_set=LED_MASK;
    *gpio_out_clear = LED_MASK;


    
    

    while (true) {
        
        current=to_ms_since_boot(get_absolute_time());

        if((current-lastHealthCheck>=1000))
        {
            
            health_task(counterHealth.overRun,counterHealth.overRunFault,counterHealth.maxExec,counterHealth.taskRuns);
            lastHealthCheck=current;
            
        }
        
        if((current-lastToggle)>=1000)
        {
            led.start=time_us_64();
            led_task(&toggleFlag);
            led.end=time_us_64();
            ledExec=led.end-led.start;
            lastToggle=current;   
            printf("Led Exec Time: %llu us\n",ledExec);   

        }

        if((current-lastCounter)>=250)
        {
            counterHealth.taskRuns++;
            counterTime.start=time_us_64();
            counter_task(&counter);
            if(injectCounterOverrun && (counterHealth.taskRuns>8 && counterHealth.taskRuns<13))
                sleep_ms(1);
            counterTime.end=time_us_64();
            counterExec=counterTime.end-counterTime.start;
            lastCounter=current;
            printf("Counter: %u\n",counter);
            printf("Counter Exec Time: %llu us\n",counterExec);
            if(counterExec>COUNTER_BUDGET_US)
            {
                counterHealth.overRun++;
                counterHealth.overRunFault = true;
                // if(recId<50)
                // {
                 record_faults(FaultOverrun,  SEVERITY_WARNING, &faultLogs);
                 print_fault_logs(&faultLogs);
                // }
                

                
            }
            
            if(counterExec>counterHealth.maxExec)
            counterHealth.maxExec=counterExec;

        }

       

        
    }
}
