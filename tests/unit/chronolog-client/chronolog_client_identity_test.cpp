#include <cstdint>

#include <gtest/gtest.h>

#include <chronolog_client.h>

using chronolog::ClientId;
using chronolog::ClientIdentity;

TEST(ClientIdentityTest, PackUnpackRoundTrip)
{
    ClientIdentity original{0x0A0B0C0Du, 0x1234u, 0x5678u};
    ClientId packed = original.pack();
    ClientIdentity recovered = ClientIdentity::unpack(packed);

    EXPECT_EQ(recovered.ip, original.ip);
    EXPECT_EQ(recovered.port, original.port);
    EXPECT_EQ(recovered.instance, original.instance);
}

TEST(ClientIdentityTest, PackLayoutMatchesSpec)
{
    // Per the documented layout in chronolog_client.h:
    //   bits [63:32] = ip, [31:16] = port, [15:0] = instance.
    ClientIdentity identity{0xDEADBEEFu, 0xCAFEu, 0xBABEu};
    ClientId packed = identity.pack();

    EXPECT_EQ(packed, 0xDEADBEEFCAFEBABEull);
}

TEST(ClientIdentityTest, ZeroIdRoundTripsToZeroFields)
{
    ClientIdentity recovered = ClientIdentity::unpack(0);
    EXPECT_EQ(recovered.ip, 0u);
    EXPECT_EQ(recovered.port, 0u);
    EXPECT_EQ(recovered.instance, 0u);
}

TEST(ClientIdentityTest, MaxValueRoundTrip)
{
    ClientIdentity max_identity{0xFFFFFFFFu, 0xFFFFu, 0xFFFFu};
    ClientId packed = max_identity.pack();
    ClientIdentity recovered = ClientIdentity::unpack(packed);

    EXPECT_EQ(packed, 0xFFFFFFFFFFFFFFFFull);
    EXPECT_EQ(recovered.ip, max_identity.ip);
    EXPECT_EQ(recovered.port, max_identity.port);
    EXPECT_EQ(recovered.instance, max_identity.instance);
}

TEST(ClientIdentityTest, WriterOnlyClientHasZeroPort)
{
    // Writer-only clients carry port 0 and rely on the pid-derived instance
    // discriminator for same-host uniqueness.
    ClientIdentity writer{0x7F000001u /* 127.0.0.1 */, 0u, 0x1234u /* pid & 0xFFFF */};
    ClientId packed = writer.pack();
    ClientIdentity recovered = ClientIdentity::unpack(packed);

    EXPECT_EQ(recovered.port, 0u);
    EXPECT_EQ(recovered.ip, 0x7F000001u);
    EXPECT_EQ(recovered.instance, 0x1234u);
}
