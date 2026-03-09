This is a BEAST of a display, appears to be 16 line RGB interface with DMA, which allows for some insane framerates (I got over 200fps on one of my tests).

To add i2c devices, use test pad "S3" for 3.3v.

Crush big black relay connector with flush cutters to slowly remove plastic, then desolder remaining metal bits.  Here you will find 5v if you need it, GND, and the unlabelled pads (for 3 relays) are GPIO 1, 2, and 40. The one close to a GND pad is GPIO40.  The one on the other side on its own is GPIO1.  The one between the two is GPIO2.   Use two of these to create a second i2c interface (touch uses first):

TwoWire sensorI2C = TwoWire(0);

Then use this to begin on desired GPIOs IE:

sensorI2C.begin(1, 40);

Backlight control is poor.  It goes through an unidentified SOT-23-6 labelled "GAS" with a triangle/swoosh logo, likely LED driver U5 as identified in 1.png.  I can't get it to work by driving it at anything other than 8bit - 10bit or 12bit and it loops through brightness levels.  Frequency doesn't seem to matter so I set it to 600hz like in the demo files.  Adjusting the backlight induces horrible coil whine out of the inductor near U5, which can be mitigated with hot glue.  Backlight turns off at levels lower than 60/255, so I set 60 as the minimum.  Crappy little LED driver.  
