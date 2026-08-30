#include <hardware/gpio.h>
#include <hardware/regs/intctrl.h>
#include <hardware/structs/io_bank0.h>
#include <hardware/timer.h>
#include <pico/error.h>
#include <pico/stdio.h>
#include <pico/time.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <hardware/uart.h>
#include <hardware/irq.h>

#include "pico/stdlib.h"
#define LED_PIN 15
#define LED_MASK (1u<<LED_PIN)
#define GPIO15_CTRL (IO_BANK0_BASE+0x07Cu)
#define GPIO_OE_SET (SIO_BASE+0x024u)
#define GPIO_OUT_SET (SIO_BASE+0x014u)
#define GPIO_OUT_CLR (SIO_BASE+0x018u)
#define FUNCSEL_MASK 0x1Fu
#define COUNTER_BUDGET_US 100u
#define FAULT_LOG_SIZE 50u
#define CMD_BUFFER_SIZE 64u
#define UART_ID uart0
#define UART_TX_PIN 0
#define UART_RX_PIN 1
#define UART_BAUD 115200
#define UART_RX_BUFFER_SIZE 128u



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

typedef struct{
    char data[UART_RX_BUFFER_SIZE];
    volatile uint32_t readIndex;
    volatile uint32_t writeIndex;
    volatile bool overflow;

}UartRxBuffer;
static UartRxBuffer uartrxBuffer={0};


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
    char output[160];
    snprintf(output, sizeof(output), "HEALTH: Total OverRuns: %u, overRunFault: %d, Max Counter Exec Time: %llu, health counter: %u \r\n", overRun,overRunFault,maxCounterExec,TaskRuns);
    uart_puts(UART_ID, output);
    // printf("HEALTH CHECK: Total OverRuns: %u, overRunFault: %d, Max Counter Exec Time: %llu, health counter: %u \n", overRun,overRunFault,maxCounterExec,TaskRuns);

}


void record_faults(FaultId id, FaultSeverity severity, FaultLogs *faultLogs)
{
    FaultRecord newRecord;
    newRecord.id=id;
    newRecord.Severity=severity;
    newRecord.timestampUs=time_us_64();
    faultLogs->faultLogsArr[faultLogs->writeIndex]=newRecord;
    faultLogs->writeIndex=(faultLogs->writeIndex+1)%FAULT_LOG_SIZE;
    // printf("Fault Record: ID:%u, Severity:%u, Time Stamp:%llu\n",newRecord.id,newRecord.Severity,newRecord.timestampUs);
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
        char output[300];
        snprintf(output,sizeof(output),"Fault Record: ID:%u, Severity:%u, Time Stamp:%llu\r\n",faultLogs->faultLogsArr[ind].id,faultLogs->faultLogsArr[ind].Severity,faultLogs->faultLogsArr[ind].timestampUs);
        // printf("Fault Record: ID:%u, Severity:%u, Time Stamp:%llu\n",faultLogs->faultLogsArr[ind].id,faultLogs->faultLogsArr[ind].Severity,faultLogs->faultLogsArr[ind].timestampUs);
        uart_puts(UART_ID, output);

    }

}

void command_line(char *cmdBuffer, taskHealth counter, FaultLogs *faultLogs, bool *injectCounterOverrun )
{
    if (strcmp(cmdBuffer,"status")==0)
    {
        health_task( counter.overRun,counter.overRunFault, counter.maxExec, counter.taskRuns);
        
    }
    else if(strcmp(cmdBuffer, "faults")==0)
    {
        print_fault_logs(faultLogs);
    }

    else if(strcmp(cmdBuffer, "inject on")==0)
    {
        *injectCounterOverrun=true;
        uart_puts(UART_ID, "Fault Injection Enabled\r\n");
    }
    else if(strcmp(cmdBuffer, "inject off")==0)
    {
        *injectCounterOverrun=false;
        uart_puts(UART_ID,"Fault Injection Disabled\r\n");
    }
    else 
    {
        // printf("Command not found\n");
        uart_puts(UART_ID, "Command not found\r\n");
    
    }

}

void uart_rx_handler(void)
{

    while(uart_is_readable(UART_ID))
    {
        char ch=uart_getc(UART_ID);

        uint32_t nextWrite=(uartrxBuffer.writeIndex+1)%UART_RX_BUFFER_SIZE;

        if(nextWrite==uartrxBuffer.readIndex)
            uartrxBuffer.overflow=true;
        else
         {
            uartrxBuffer.data[uartrxBuffer.writeIndex]=ch;
            uartrxBuffer.writeIndex=nextWrite;
         };
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
    bool injectCounterOverrun = false;
    char cmdBuffer[CMD_BUFFER_SIZE];
    uint32_t cmdInd=0;

    
    volatile uint32_t *reg15=(volatile uint32_t *) GPIO15_CTRL;
    uint32_t reg=*reg15;
    reg&=~FUNCSEL_MASK;
    reg|=5u; 
    *reg15=reg;
    *gpio_oe_set=LED_MASK;
    *gpio_out_clear = LED_MASK;

    uart_init(UART_ID, UART_BAUD);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
    irq_set_exclusive_handler(UART0_IRQ, uart_rx_handler);
    uart_set_irq_enables(UART_ID, true, false);
    irq_set_enabled(UART0_IRQ, true);




    while (true) {
        
        current=to_ms_since_boot(get_absolute_time());
        
        if((current-lastToggle)>=1000)
        {
            led.start=time_us_64();
            led_task(&toggleFlag);
            led.end=time_us_64();
            ledExec=led.end-led.start;
            lastToggle=current;  

        }

        if((current-lastCounter)>=250)
        {
            counterHealth.taskRuns++;
            counterTime.start=time_us_64();
            counter_task(&counter);
            if(injectCounterOverrun)
                sleep_ms(1);
            counterTime.end=time_us_64();
            counterExec=counterTime.end-counterTime.start;
            lastCounter=current;
            if(counterExec>COUNTER_BUDGET_US)
            {
                counterHealth.overRun++;
                counterHealth.overRunFault = true;

                 record_faults(FaultOverrun,  SEVERITY_WARNING, &faultLogs);
            
            }            
            if(counterExec>counterHealth.maxExec)
            counterHealth.maxExec=counterExec;

        }

        // int ch=getchar_timeout_us(0);
        // if(ch!=PICO_ERROR_TIMEOUT)
        //  {
        //     char chr=(char)ch;
        //     if((chr=='\r')||(chr=='\n'))
        //     {        
        //         if(cmdInd>0)
        //         {
        //             cmdBuffer[cmdInd]='\0';
        //             command_line(cmdBuffer, counterHealth, &faultLogs, &injectCounterOverrun);
        //             cmdInd=0;
        //         }
        //     }
        //     else 
        //     {
        //         if(cmdInd<CMD_BUFFER_SIZE-1)
        //         {
        //             cmdBuffer[cmdInd]=chr;
        //             cmdInd++;
        //         }

        //     }
            

        //  }

        while(uartrxBuffer.readIndex!=uartrxBuffer.writeIndex)
        {
            char uartCh=uartrxBuffer.data[uartrxBuffer.readIndex];
            uartrxBuffer.readIndex=(uartrxBuffer.readIndex+1)%UART_RX_BUFFER_SIZE;

            uart_putc_raw(UART_ID, uartCh);

            if(uartCh=='\r'||uartCh=='\n')
            {
                if(cmdInd>0)
                {
                    cmdBuffer[cmdInd]='\0';
                    command_line(cmdBuffer, counterHealth, &faultLogs, &injectCounterOverrun);
                    cmdInd=0;
                }

            }
            else 
            {
                if(cmdInd<CMD_BUFFER_SIZE-1)
                {
                    cmdBuffer[cmdInd]=uartCh;
                    cmdInd++;

                }
                
            
            }

        }
    }
}
