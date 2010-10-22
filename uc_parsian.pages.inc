<?php
// $Id$

/**
 * @file
 * Ubercart Parsian payment menu callbacks and their helper functions.
 */

/**
 * Order payment request page.
 */
function uc_parsian_callback_request() {
  // Loading order data.
  global $base_root;
  $order_id = intval($_SESSION['cart_order']);
  $order = uc_order_load($order_id);
  
  // Preparing for SOAP call.
  $soap_params = array(
    'status' => 1,
    'authority' => 0,
    'orderId' => $order->order_id,
    'amount' => intval($order->order_total),
    'callbackUrl' => $base_root . url('cart/parsian/complete'),
    'pin' => variable_get('uc_parsian_auth_pin', ''),
  );
  $soap_results = _uc_parsian_soap_call('PinPaymentRequest', $soap_params);
  
  // Checking SOAP call errors.
  if ($soap_results['#error'] !== FALSE || empty($soap_results['#return']['authority']) || $soap_results['#return']['status'] != 0) {
    _uc_parsian_log_error('Could not request pin payment. Status: @code', array('@code' => _uc_parsian_error_translate($soap_results['#return']['status'])));
  }
  
  // Saving SOAP results, redirecting to Parsian Payment gateway.
  $authority = $soap_results['#return']['authority'];
  $gateway = variable_get('uc_parsian_gateway', 'https://www.pec24.com/pecpaymentgateway/');
  
  // Store authority code for future validations.
  $record = new stdClass();
  $record->oid = $order_id;
  $record->authority = $authority;
  $record->created = time();
  drupal_write_record('uc_payment_parsian', $record);
  
  drupal_goto($gateway . '?' . http_build_query(array('au' => $authority)));
  exit();
}


/**
 * Order payment completion page.
 */
function uc_parsian_callback_complete($cart_id = 0) {
  
}

/**
 * Helper function to call a remote SOAP method.
 *
 * @param $soap_method
 *   Remote SOAP method to call.
 * @param $soap_params
 *   SOAP parameters.
 * @param $soap_options
 *   SOAP call options.
 *
 * @return
 *   SOAP call results.
 */
function _uc_parsian_soap_call($soap_method, $soap_params, $soap_options = NULL) {
  // Settings default SOAP options, if not yet set.
  if (is_null($soap_options)) {
    $soap_options = array(
      'style' => 'rpc',
      'use' => 'encoded',
    );
  }
  
  // Getting a DrupalSoapClient instance.
  $soap_server = variable_get('uc_parsian_soap_server', 'https://www.pec24.com/pecpaymentgateway/eshopservice.asmx?wsdl');
  $soap_client = soapclient_init_client($soap_server, TRUE, $soap_options);

  // Checking instantiation errors.
  if (!empty($soap_client['#error'])) {
    _uc_parsian_log_error('Could not connect to Parsian webservice.');
  }
  
  return $soap_client['#return']->call($soap_method, $soap_params);
}

/**
 * Helper function to display and log uc_parsian errors.
 *
 * @param $message
 *   Error message to be logged.
 * @param $variables
 *   Array to be placed on placeholders in $message.
 * @param $redirect
 *   Drupal path to redirect to after error logging.
 *   Set to FALSE to disable redirection.
 */
function _uc_parsian_log_error($message, $variables = array(), $redirect = 'cart/checkout') {
  global $_uc_parsian_watchdog_log;
  
  // Sanitize variables.
  if (!empty($variables)) {
    $variables = array_map('check_plain', $variables);
  }
  
  // Display error to the end user.
  drupal_set_message(t($message, $variables), 'error');
  
  // Log to Watchdog if configured so.
  if ($_uc_parsian_watchdog_log) {
    watchdog('uc_parsian', $message, $variables, WATCHDOG_ERROR);
  }
  
  // Redirect if necessary.
  if ($redirect) {
    drupal_goto($redirect);
    exit();
  }
}

/**
 * Helper function to translate error status codes.
 *
 * @param $code
 *   Parsian payment status code.
 *
 * @return
 *   Translated string.
 */
function _uc_parsian_error_translate($code) {
  switch ($code) {
    case 20:
    case 22:
      return $code . ' - ' . t('Invalid PIN or IP.');
    
    case 30:
      return $code . ' - ' . t('The operation has been done previously.');
    
    case 34:
      return $code . ' - ' . t('Invalid transaction number.');
    
    default:
      return $code . ' - ' . t('Unknown status code. Refer to your technical documentation or contact Parsian bank support.');
  }
}
