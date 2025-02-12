/*
* Copyright (c) 2005-2025 Igor Popov <ipopovi@gmail.com> 
*
* $Id: tilde_users.sql 38 2008-07-15 07:27:33Z igor_popov $
*
*/

CREATE DATABASE `hosting`;
USE `hosting`;

CREATE TABLE `tilde_users` (
    `username` varchar(255) NOT NULL default '',
    `homedir` varchar(255) NOT NULL default '',
    `enabled` enum('yes','no') NOT NULL default 'no',
    `extra_php_config` text,
    PRIMARY KEY  (`username`),
    KEY (`enabled`)
) COMMENT='hosting ~users';

CREATE USER 'nonpriv'@'localhost' IDENTIFIED BY 'M3Ga_PaSsVVd';
GRANT SELECT ON `hosting`.`tilde_users` TO 'nonpriv'@'localhost';
FLUSH PRIVILEGES;

INSERT INTO `tilde_users` VALUES ('igor', '/var/www/homedirs/igor', 'yes', 'safe_mode=1');
INSERT INTO `tilde_users` VALUES ('someone', '/var/www/homedirs/someone', 'yes', 'register_globals=1');
INSERT INTO `tilde_users` VALUES ('also_someone1', '/var/www/homedirs/someone', 'no', 'engine=0');

COMMIT;
