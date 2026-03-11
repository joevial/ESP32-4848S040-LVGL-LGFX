This is a BEAST of a display, appears to be 16 line RGB interface with DMA, which allows for some insane framerates (I got over 200fps on one of my tests).

To add i2c devices, use test pad "S3" for 3.3v.

Crush big black relay connector with flush cutters to slowly remove plastic, then desolder remaining metal bits.  Here you will find 5v if you need it, GND, and the unlabelled pads (for 3 relays) are GPIO 1, 2, and 40. The one close to a GND pad is GPIO40.  The one on the other side on its own is GPIO1.  The one between the two is GPIO2.   Use two of these to create a second i2c interface (touch uses first):

TwoWire sensorI2C = TwoWire(0);

Then use this to begin on desired GPIOs IE:

sensorI2C.begin(1, 40);

Backlight control is poor.  It goes through an unidentified SOT-23-6 labelled "GAS" with a triangle/swoosh logo, likely LED driver U5 as identified in 1.png.  I can't get it to work by driving it at anything other than 8bit - 10bit or 12bit and it loops through brightness levels.  Backlight turns off at levels lower than 60/255, so I set 60 as the minimum.  Crappy little LED driver.  Coil whine was eliminated with ledcdetach and a second ledcattach at 100khz - doing it once doesn't take, you have to detach and reattach for it to actually be at 100khz.  then the coil whine is completely gone at any brightness, hooray!

Backlight driver identified as  DIODES™ AP3031.

<img width="673" height="902" alt="image" src="https://github.com/user-attachments/assets/cd3ee108-8fad-4060-994e-dff244fed371" />

Unfortunately, the built in GPIO 38 doesn't allow for a full range of brightness at the frequency high enough to eliminate coil whine.  So I fixed that too.  I removed r12, the 5.1ohm resistor, and replaced it with a 33 ohm resistor.  Then I added an 80nf capacitor from this same pin (pin 3, FB, of the AP3031) to GND, and a wire from this same pin to GPIO 2.  Then we can use GPIO 2 and GPIO 38 as a sort of coarse and fine brightness knobs.

```
void setBrightness(uint8_t brightness) {
    uint8_t en_duty, fb_duty;
    if (brightness >= 128) {
        // Upper half: FB at full brightness, sweep EN from min to max
        float t  = (brightness - 128) / 127.0f;
        en_duty  = (uint8_t)(BL_EN_MIN + t * (BL_EN_MAX - BL_EN_MIN) + 0.5f);
        fb_duty  = BL_FB_BRIGHT;
    } else {
        // Lower half: EN at max, sweep FB from bright to dim (inverted)
        float t  = brightness / 127.0f;
        en_duty  = BL_EN_MAX;
        fb_duty  = (uint8_t)(BL_FB_DIM - t * (BL_FB_DIM - BL_FB_BRIGHT) + 0.5f);
    }
    ledcWrite(GFX_BL,    en_duty);
    ledcWrite(GFX_BL_FB, fb_duty);
}

```

