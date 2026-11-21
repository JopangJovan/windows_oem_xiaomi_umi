/* Diagnostic-only: replaces broad SPMI ownership with three observer resources.
 * Keep the working touchscreen SSDT alongside this overlay.
 */
DefinitionBlock ("", "SSDT", 2, "UMI", "BUTTONS", 1)
{
    External (\_SB.SPMI, DeviceObj)
    Scope (\_SB.SPMI) { Name (_STA, Zero) }
    Scope (\_SB)
    {
        Device (UBTN)
        {
            Name (_HID, "UMI0004")
            Name (_UID, Zero)
            Name (_CCA, Zero)
            Name (_STA, 0x0F)
            Name (_CRS, ResourceTemplate () {
                Memory32Fixed (ReadWrite, 0x0C440000, 0x2000)
                Memory32Fixed (ReadWrite, 0x0C40A000, 0x1000)
                Memory32Fixed (ReadWrite, 0x0E600000, 0x10000)
            })
        }
    }
}
