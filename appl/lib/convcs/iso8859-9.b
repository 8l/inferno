implement GenCP;

include "sys.m";
include "draw.m";
include "gencp.b";

CHARSET : con "iso-8859-9";

cstab := array [] of {
	16r0000,16r0001,16r0002,16r0003,16r0004,16r0005,16r0006,16r0007,
	16r0008,16r0009,16r000a,16r000b,16r000c,16r000d,16r000e,16r000f,
	16r0010,16r0011,16r0012,16r0013,16r0014,16r0015,16r0016,16r0017,
	16r0018,16r0019,16r001a,16r001b,16r001c,16r001d,16r001e,16r001f,
	16r0020,16r0021,16r0022,16r0023,16r0024,16r0025,16r0026,16r0027,
	16r0028,16r0029,16r002a,16r002b,16r002c,16r002d,16r002e,16r002f,
	16r0030,16r0031,16r0032,16r0033,16r0034,16r0035,16r0036,16r0037,
	16r0038,16r0039,16r003a,16r003b,16r003c,16r003d,16r003e,16r003f,
	16r0040,16r0041,16r0042,16r0043,16r0044,16r0045,16r0046,16r0047,
	16r0048,16r0049,16r004a,16r004b,16r004c,16r004d,16r004e,16r004f,
	16r0050,16r0051,16r0052,16r0053,16r0054,16r0055,16r0056,16r0057,
	16r0058,16r0059,16r005a,16r005b,16r005c,16r005d,16r005e,16r005f,
	16r0060,16r0061,16r0062,16r0063,16r0064,16r0065,16r0066,16r0067,
	16r0068,16r0069,16r006a,16r006b,16r006c,16r006d,16r006e,16r006f,
	16r0070,16r0071,16r0072,16r0073,16r0074,16r0075,16r0076,16r0077,
	16r0078,16r0079,16r007a,16r007b,16r007c,16r007d,16r007e,16r007f,
	16r0080,16r0081,16r0082,16r0083,16r0084,16r0085,16r0086,16r0087,
	16r0088,16r0089,16r008a,16r008b,16r008c,16r008d,16r008e,16r008f,
	16r0090,16r0091,16r0092,16r0093,16r0094,16r0095,16r0096,16r0097,
	16r0098,16r0099,16r009a,16r009b,16r009c,16r009d,16r009e,16r009f,
	16r00a0,16r00a1,16r00a2,16r00a3,16r00a4,16r00a5,16r00a6,16r00a7,
	16r00a8,16r00a9,16r00aa,16r00ab,16r00ac,16r00ad,16r00ae,16r00af,
	16r00b0,16r00b1,16r00b2,16r00b3,16r00b4,16r00b5,16r00b6,16r00b7,
	16r00b8,16r00b9,16r00ba,16r00bb,16r00bc,16r00bd,16r00be,16r00bf,
	16r00c0,16r00c1,16r00c2,16r00c3,16r00c4,16r00c5,16r00c6,16r00c7,
	16r00c8,16r00c9,16r00ca,16r00cb,16r00cc,16r00cd,16r00ce,16r00cf,
	16r011e,16r00d1,16r00d2,16r00d3,16r00d4,16r00d5,16r00d6,16r00d7,
	16r00d8,16r00d9,16r00da,16r00db,16r00dc,16r0130,16r015e,16r00df,
	16r00e0,16r00e1,16r00e2,16r00e3,16r00e4,16r00e5,16r00e6,16r00e7,
	16r00e8,16r00e9,16r00ea,16r00eb,16r00ec,16r00ed,16r00ee,16r00ef,
	16r011f,16r00f1,16r00f2,16r00f3,16r00f4,16r00f5,16r00f6,16r00f7,
	16r00f8,16r00f9,16r00fa,16r00fb,16r00fc,16r0131,16r015f,16r00ff,
};