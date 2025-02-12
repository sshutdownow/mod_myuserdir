/*
* Copyright (c) 2005 Igor Popov <igorpopov@newmail.ru> 
*
* $Id: tilde_users.sql 27 2007-08-05 20:10:51Z igor_popov $
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
) ENGINE=MyISAM COMMENT='hosting ~users';

GRANT SELECT ON `hosting`.`tilde_users` TO 'nonpriv'@'localhost' IDENTIFIED BY 'M3Ga PaSsVVd';

INSERT INTO `tilde_users` VALUES ('igor', '/var/www/homedirs/igor', 'yes', 'safe_mode=1');
INSERT INTO `tilde_users` VALUES ('someone', '/var/www/homedirs/someone', 'yes', 'register_globals=1');
INSERT INTO `tilde_users` VALUES ('also_someone1', '/var/www/homedirs/someone', 'no', 'engine=0');

COMMIT;
