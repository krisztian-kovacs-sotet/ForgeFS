#include <gtest/gtest.h>

#include "server/SessionManager.hpp"

using forgefs::server::SessionManager;

TEST(SessionManagerTest, CreateSessionThenLookupReturnsUsername) {
    SessionManager sessions;
    const std::string token = sessions.CreateSession("alice");
    EXPECT_FALSE(token.empty());

    const auto username = sessions.UsernameForToken(token);
    ASSERT_TRUE(username.has_value());
    EXPECT_EQ(*username, "alice");
}

TEST(SessionManagerTest, UnknownTokenReturnsNullopt) {
    SessionManager sessions;
    EXPECT_FALSE(sessions.UsernameForToken("not-a-real-token").has_value());
}

TEST(SessionManagerTest, EachLoginGetsAFreshToken) {
    SessionManager sessions;
    const std::string t1 = sessions.CreateSession("alice");
    const std::string t2 = sessions.CreateSession("alice");
    EXPECT_NE(t1, t2);
    EXPECT_EQ(*sessions.UsernameForToken(t1), "alice");
    EXPECT_EQ(*sessions.UsernameForToken(t2), "alice");
}
