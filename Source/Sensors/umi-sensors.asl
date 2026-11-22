DefinitionBlock ("", "SSDT", 2, "XIAOMI", "UMISNS  ", 0x00000001)
{
    Scope (\_SB)
    {
        Device (UMSN)
        {
            Name (_HID, "UMI0009")
            Name (_UID, Zero)
            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }
    }
}
