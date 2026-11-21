DefinitionBlock ("", "SSDT", 2, "XIAOMI", "UMIBATT ", 0x00000001)
{
    Scope (\_SB)
    {
        Device (UBAT)
        {
            Name (_HID, "UMI0006")
            Name (_UID, Zero)
            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }
    }
}
