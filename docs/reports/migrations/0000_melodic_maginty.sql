CREATE TABLE `frames` (
	`hash` text PRIMARY KEY NOT NULL,
	`frame` text NOT NULL
);
--> statement-breakpoint
CREATE TABLE `report_frames` (
	`report_id` text NOT NULL,
	`frame_index` integer NOT NULL,
	`frame_hash` text NOT NULL,
	PRIMARY KEY(`report_id`, `frame_index`),
	FOREIGN KEY (`report_id`) REFERENCES `reports`(`id`) ON UPDATE no action ON DELETE cascade,
	FOREIGN KEY (`frame_hash`) REFERENCES `frames`(`hash`) ON UPDATE no action ON DELETE restrict
);
--> statement-breakpoint
CREATE TABLE `reports` (
	`id` text PRIMARY KEY NOT NULL,
	`runtime` text NOT NULL,
	`version` text NOT NULL,
	`trace` text NOT NULL,
	`kind` text NOT NULL,
	`reason` text NOT NULL,
	`code` text NOT NULL,
	`target` text NOT NULL,
	`os` text NOT NULL,
	`arch` text NOT NULL,
	`fault_address` text NOT NULL,
	`elapsed_ms` integer,
	`peak_rss` integer,
	`first_seen_at` text NOT NULL,
	`last_seen_at` text NOT NULL,
	`hit_count` integer NOT NULL,
	`expires_at` text NOT NULL
);
