import time
import serial

def send_command(ser,command):
    ser.write((command+"\r\n").encode())
    ser.flush()
    response=b"";
    start=time.monotonic();
    last_read=start;
    data_read=False
    while(time.monotonic()-start<1.0):
        if(ser.in_waiting):
            data=ser.read(ser.in_waiting)
            response+=data
            last_read=time.monotonic()
            data_read=True

        if(data_read and (time.monotonic()-last_read)>=0.1):
            break;
        
        time.sleep(0.01);

    response=response.decode()
    return response
    




def parse_status(response):
    health_line= None
    status_dict={}
    lines=response.splitlines()
    for line in lines:
        if(line.startswith("HEALTH:")):
            health_line=line
            break
    if(health_line is None):
        raise ValueError("HEALTH line not found")
    health_line=health_line.removeprefix("HEALTH:").strip()
    fields=health_line.split(",")
    for field in fields:
        key,value=field.split(":",1)
        key=key.strip()
        try:
            value=int(value.strip())
        except ValueError:
            raise ValueError(f" Invalid numeric value in: {field}")
        status_dict[key]=value
    
    return status_dict



def test_overrun_detection(ser):
    before_response=send_command(ser,"status")
    before=parse_status(before_response)
    before_overrun= before["Total OverRuns"]
    send_command(ser,"inject on")
    try:
        time.sleep(1.2)
    finally:
        send_command(ser,"inject off")
    after_response=send_command(ser,"status")
    after= parse_status(after_response)
    after_overrun=after["Total OverRuns"]

    assert after_overrun>before_overrun,(f"Overrun did not increase:" 
    f"{before_overrun} --> {after_overrun}\n")  

    print(f"[PASS] Overrun detection : {before_overrun} --> {after_overrun} ") 


def wait_for_messages(ser,command, expected_messages, timeout):
    ser.reset_input_buffer()
    ser.write((command+"\r\n").encode())
    ser.flush()
    response=b""
    start=time.monotonic()
    while(time.monotonic()-start<=timeout):
        if(ser.in_waiting):
            data=ser.read(ser.in_waiting)
            response+=data
        response_text=response.decode(errors="replace")
        normalized_text=response_text.lower()
        if all(message.lower() in normalized_text for message in expected_messages):
            return response_text

        time.sleep(0.01)
    raise TimeoutError(f"Timeout Error: {expected_messages}")
    

def test_stall_recovery(ser):
    before_status=send_command(ser,"status")
    before_parsed=parse_status(before_status)
    before_recoveries=before_parsed["recoveries"]

    wait_for_messages(ser,"stall on", ["Automatic recovery initiated","System recovered successfully"], 3)

    after_status=send_command(ser,"status")
    after_parsed=parse_status(after_status)
    after_recoveries=after_parsed["recoveries"]
    recovery_mode=after_parsed["recovery mode"]

    assert after_recoveries>before_recoveries, (f"Recovery count did not increase: "
    f"{before_recoveries} --> {after_recoveries}")
    assert recovery_mode==0, (f"System remained in recovery mode: {recovery_mode}")
    print(f"[PASS] Stall recovery: recoveries "
    f"{before_recoveries} --> {after_recoveries}, "
    f"recovery mode={recovery_mode}")


def test_watchdog_recovery(ser):
    before_status=send_command(ser,"status")
    parsed_before=parse_status(before_status)
    wait_for_messages(ser,"hang",["BOOT: Previous reset caused by watchdog"],5)
    synchronize_parser(ser)
    after_status=send_command(ser,"status")
    # print(f"Post-watchdog status response: {after_status!r}")
    parsed_after=parse_status(after_status)
    print(
        f"[PASS] Watchdog recovery: "
        f"firmware rebooted and responded to status, "
        f"health counter={parsed_after['health counter']}"
    )

def synchronize_parser(ser):
    time.sleep(0.2)
    ser.write(b"\r\n") # Synchronize command parser before first command
    ser.flush()
    time.sleep(0.1)
    ser.reset_input_buffer()

with serial.Serial("/dev/cu.usbserial-A5069RR4",115200,timeout=0.1) as ser:
    synchronize_parser(ser)
    response=send_command(ser,"status")
    print(f"Data recieved from Pico:{response}")
    try:
        status=parse_status(response)
    except ValueError as error:
        print(f"[FAIL] Status Parsing {error}");
    else:
        print(f"[PASS] Status parsing succesful: {status}")
    try:
        test_overrun_detection(ser)
    except ValueError as error:
        print(f"[FAIL] Overrun detection: {error}")
    except AssertionError as error:
        print(f"[FAIL] Overrun detection: {error}")


    try:
        test_stall_recovery(ser)
    except TimeoutError as error:
        print(f"[FAIL] Stall Recovery Timeout: {error}")
    except AssertionError as error:
        print(f"[FAIL] Stall Recovery: {error}")
    except ValueError as error:
        print(f"[FAIL] Stall recovery parsing: {error}")

    try:
        test_watchdog_recovery(ser)
    except TimeoutError as error:
        print(f"[FAIL] Watchdog Timeout: {error}")
    except ValueError as error:
        print(f"[FAIL] Watchdog post-reset status: {error}")

    