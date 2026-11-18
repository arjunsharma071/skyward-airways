CREATE TABLE IF NOT EXISTS skyward_schema_migrations (
    version INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    applied_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS skyward_records (
    record_type TEXT NOT NULL
        CHECK (record_type IN ('flights', 'passengers', 'bookings')),
    record_key TEXT NOT NULL,
    record_data TEXT NOT NULL,
    ordinal BIGINT NOT NULL CHECK (ordinal >= 0),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (record_type, record_key)
);

CREATE TABLE IF NOT EXISTS skyward_legacy_imports (
    source_name TEXT PRIMARY KEY,
    imported_rows BIGINT NOT NULL CHECK (imported_rows >= 0),
    imported_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

INSERT INTO skyward_schema_migrations (version, name)
VALUES (1, 'initial record store and legacy import tracking')
ON CONFLICT (version) DO NOTHING;
