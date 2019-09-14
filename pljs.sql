CREATE FUNCTION pljs_call_handler() RETURNS language_handler
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION pljs_call_validator(oid) RETURNS void
	AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE TRUSTED LANGUAGE pljs
 HANDLER pljs_call_handler
 VALIDATOR pljs_call_validator;
