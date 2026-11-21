/* SPDX-License-Identifier: BSD-2-Clause-Patent */
DefinitionBlock ("", "SSDT", 2, "UMI", "AUDIO", 0x00000001)
{
    Scope (\_SB)
    {
        Device (UAUD)
        {
            Name (_HID, "UMI0005")
            Name (_UID, Zero)
            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }
    }
}
