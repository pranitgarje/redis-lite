from aphelo import ApheloClient

# Connect to the database
db = ApheloClient()

# Test basic caching
print("Setting data...")
db.set("user:1001", "Pranit")
print("Retrieved:", db.get("user:1001"))

# Test your new TTL Cache Expiration!
print("\nSetting temp_token with 2-second expiration...")
db.set("temp_token", "secret_abc123")
db.pexpire("temp_token", 2000)

import time
print("Checking token immediately:", db.get("temp_token"))
print("Waiting 2.5 seconds...")
time.sleep(2.5)
print("Checking token after expiration:", db.get("temp_token"))

# Test the Sorted Set
print("\nAdding to Leaderboard...")
db.zadd("leaderboard", 150.5, "Alice")
db.zadd("leaderboard", 200.0, "Bob")
print("Leaderboard added successfully!")