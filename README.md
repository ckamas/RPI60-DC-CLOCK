# RPI60-DC-CLOCK

This is some simple code I wrote to drive the front panel displace of a True Time Model 60-DC WWVB reciever. Unfortunatly since the modulation scheme of WWVB has been changed, this clock no longer syncronizes its time. So I salvaged the display and wrote some code to drive it with a NTP synced RPI.

I wrote this as a cheat sheet for me to remember in a few years when I have forgotten what I did today.

Chuck
Christmas 2020


Installation:
1) clone the latest RPI lite image found at:
https://www.raspberrypi.org/software/operating-systems/#raspberry-pi-os-32-bit

2. boot and do all of the updates/upgrades

3. enable ssh and change the password.

4. Enable SAMBA:
https://www.raspberrypi.org/documentation/remote-access/samba.md

4.1 edit /etc/samba/smb.conf and change the home directory share to RW. I did this to I could use a real editor on my PC.

4.2 set smb password:
sudo smbpasswd -a pi

5. install wiring pi
sudo apt-get install wiringpi 

6. install git

7. clone the repository

8. kick off make.sh

9. add quality of ntp to contol the front panel led
sudo apt-get install ntpstat

optional:
1) make filesystem read only:

--- run this script:
https://learn.adafruit.com/read-only-raspberry-pi/overview

--- modify ntp:
https://unix.stackexchange.com/questions/553930/ntp-wont-work-on-read-only-raspberry-pi

