// 7-segment bit masks, one for each decimal digit.
static const uint kDigit[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66,
                                0x6D, 0x7D, 0x07, 0x7F, 0x6F};

// Digit cell is 4x7 units, slot pitch 5 for a 1 unit gap.
float seg_coverage(uint mask, float2 lp)
{
    if (lp.x < 0.0 || lp.x >= 4.0 || lp.y < 0.0 || lp.y >= 7.0) return 0.0;
    bool on = false;
    if ((mask & 0x01u) != 0u && lp.y < 1.0 && lp.x >= 0.5 && lp.x < 3.5)
        on = true;
    if ((mask & 0x40u) != 0u && lp.y >= 3.0 && lp.y < 4.0 &&
        lp.x >= 0.5 && lp.x < 3.5) on = true;
    if ((mask & 0x08u) != 0u && lp.y >= 6.0 && lp.x >= 0.5 && lp.x < 3.5)
        on = true;
    if ((mask & 0x20u) != 0u && lp.x < 1.0 && lp.y >= 0.5 && lp.y < 3.5)
        on = true;
    if ((mask & 0x02u) != 0u && lp.x >= 3.0 && lp.y >= 0.5 && lp.y < 3.5)
        on = true;
    if ((mask & 0x10u) != 0u && lp.x < 1.0 && lp.y >= 3.5 && lp.y < 6.5)
        on = true;
    if ((mask & 0x04u) != 0u && lp.x >= 3.0 && lp.y >= 3.5 && lp.y < 6.5)
        on = true;
    return on ? 1.0 : 0.0;
}
