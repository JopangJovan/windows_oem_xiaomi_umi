DefinitionBlock ("", "SSDT", 2, "XIAOMI", "UMIBT   ", 0x00000001)
{
    Scope (\_SB)
    {
        Device (UMBT)
        {
            Name (_HID, "UMI0007")
            Name (_UID, Zero)
            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
            Method (_CRS, 0, NotSerialized)
            {
                Name (RBUF, ResourceTemplate ()
                {
                    Memory32Fixed (ReadWrite,
                        0x00998000,         // QUP0 SE6 (GENI UART6)
                        0x00004000,
                        )
                    Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive, ,, )
                    {
                        0x0000027F,         // GIC SPI 607
                    }
                })
                Return (RBUF)
            }
        }
    }
}
