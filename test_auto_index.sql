-- Test Script for Autonomous Indexing
-- Run this with psql: psql -f test_auto_index.sql

-- Disable statement logging to reduce noise
-- Disable statement logging to reduce noise
SET log_statement = 'none';

-- Set the number of iterations for the test loops
-- You can change this value to control how many queries are run
SET auto_index.iter_count = 5;

-- 1. Create a table with enough data to trigger the "Table Size" check (> 1000 rows)
DROP TABLE IF EXISTS test_users;
CREATE TABLE test_users (
    id SERIAL PRIMARY KEY,
    username TEXT,
    email TEXT,
    age INT,
    gender CHAR(1),
    created_at TIMESTAMP DEFAULT NOW()
);

-- 2. Insert 10,000 rows
-- We make 'username' unique-ish, 'gender' low cardinality (M/F)
INSERT INTO test_users (username, email, age, gender)
SELECT 
    'user_' || i, 
    'user_' || i || '@example.com', 
    (random() * 80)::INT,
    CASE WHEN (random() > 0.5) THEN 'M' ELSE 'F' END
FROM generate_series(1, 10000) AS i;

-- 3. Analyze to populate pg_statistic (Crucial for the ML model)
ANALYZE test_users;

-- 4. Run queries to trigger the tracker

\echo 'Running queries on username (High Selectivity)...'
DO $$
DECLARE
    i INT;
BEGIN
    FOR i IN 1..current_setting('auto_index.iter_count')::int LOOP
        PERFORM * FROM test_users WHERE username = 'user_500';
    END LOOP;
END $$;

\echo 'Running queries on gender (Low Selectivity - Should NOT index)...'
DO $$
DECLARE
    i INT;
BEGIN
    FOR i IN 1..current_setting('auto_index.iter_count')::int LOOP
        PERFORM * FROM test_users WHERE gender = 'M';
    END LOOP;
END $$;

\echo 'Running composite queries on username AND gender (Composite Index)...'
DO $$
DECLARE
    i INT;
BEGIN
    -- Reduced from 60 to 5 for demo
    FOR i IN 1..current_setting('auto_index.iter_count')::int LOOP
        PERFORM * FROM test_users WHERE username = 'user_500' AND gender = 'M';
    END LOOP;
END $$;

-- 5. Wait for the background worker
\echo 'Queries done. The worker runs every 5 seconds.'
\echo 'Waiting 10 seconds...'
SELECT pg_sleep(10);

-- Check if index exists
SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'test_users';
