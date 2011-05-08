<?php

/**
 * @file
 * Contains Ubercart Parsian payment menu callbacks & internal helpers.
 *
 * @todo
 *   - Improve verification callback.
 */

/**
 * Order payment request page.
 */
function uc_parsian_callback_request() {
  global $base_root;
  // Load order data.
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
    _uc_parsian_log_error(t('Could not request pin payment. Status: @code',
      array('@code' => _uc_parsian_error_translate($soap_results['#return']['status']))
    ));
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

  // Run into that.
  drupal_goto($gateway . '?' . http_build_query(array('au' => $authority)));
}


/**
 * Order payment completion page.
 */
function uc_parsian_callback_complete($cart_id = 0) {
  $status = (int) $_REQUEST['rs'];
  $authority = (int) $_REQUEST['au'];

  // Checking authority/status availability and validatability.
  if (!isset($status, $authority) || !_uc_parsian_authority_verify($authority)) {
    _uc_parsian_log_error(t('Could not recieve valid authorities and status codes from Parsian bank. Operation cancelled.'));
  }

  // When an order status is not in "in_checkout" state,
  // do not validate the payment by sending an enquiry to the bank.
  $order = _uc_parsian_order_get($authority);
  if ($order === FALSE || uc_order_status_data($order->order_status, 'state') != 'in_checkout') {
    // Delete the orphan failed authority record.
    _uc_parsian_authority_delete($authority);
    _uc_parsian_log_error(t('The order status is not in checkout. You are not authorized to checkout this order.'));
  }

  // Parsian payment enquiry.
  $soap_params = array(
    'status' => $status,
    'authority' => $authority,
    'pin' => variable_get('uc_parsian_auth_pin', ''),
  );
  $soap_results = _uc_parsian_soap_call('PinPaymentEnquiry', $soap_params);

  // Checking SOAP enquiry call results for errors.
  if ($soap_results['#error'] !== FALSE || $soap_results['#return']['status'] != 0) {
    // Delete the orphan failed authority record.
    _uc_parsian_authority_delete($authority);
    _uc_parsian_log_error(t('Could not validate your payment, Enquiry failed. Status: @code',
      array('@code' => _uc_parsian_error_translate($soap_results['#return']['status']))
    ));
  }

  // If we're here, the payment was successfull, Finalize!
  _uc_parsian_order_finalize($order, $authority, $cart_id);
}

/**
 * Finalizes a successfull paid order.
 *
 * @param $order
 *   Order object.
 * @param $authority
 *   Payment's valid authority code.
 * @param $cart_id
 *   Cart identifier to get emptied.
 */
function _uc_parsian_order_finalize($order, $authority, $cart_id) {
  // Mark the payment authority record as valid.
  _uc_parsian_authority_validate($authority);

  // Enter payment info alongside a comment.
  $comment = t('Total amount of "!amount" has been successfully paid via !method. Payment transaction number is "!authority".',
    array(
      '!amount' => uc_currency_format($order->order_total, TRUE, TRUE, NULL),
      '!method' => ucwords($order->payment_method),
      '!authority' => $authority,
    )
  );
  // Enter payment for the order.
  uc_payment_enter($order->order_id, $order->payment_method, $order->order_total, 0, array('authority' => $authority), $comment);
  // Let her know the reference number.
  drupal_set_message($comment);
  // Empty user cart, Update order status, register customer if required
  // and log her in. Also get the themed output of order completion page.
  $message = uc_cart_complete_sale($order, TRUE);
  // Notify user about order completion.
  drupal_set_message($message);
  // Log comments to order, for both customer & admin.
  // Needs to be called after uc_cart_complete_sale().
  uc_order_comment_save($order->order_id, 0, $comment);
  uc_order_comment_save($order->order_id, 0, $comment, 'order', uc_order_state_default('post_checkout'), TRUE);
  // And redirect to the proper end!
  drupal_goto(variable_get('uc_cart_checkout_complete_page', 'cart'));
}

/**
 * Gets order identifier by authority code.
 *
 * @param $authority
 *   Authority code from Parsian payment.
 *
 * @return
 *   Order object or FALSE on failure.
 */
function _uc_parsian_order_get($authority) {
  $oid = db_result(db_query("SELECT oid FROM {uc_payment_parsian} WHERE authority = '%s'", $authority));
  return $oid ? uc_order_load($oid) : FALSE;
}

/**
 * Checks coming authority code to be equal to the local one.
 *
 * @param $authority
 *   Coming authority code to be verified.
 *
 * @return
 *   TRUE or FALSE based on the verification.
 */
function _uc_parsian_authority_verify($authority) {
  $authority_local = db_result(db_query("SELECT COUNT(*) FROM {uc_payment_parsian} WHERE authority = '%s'", $authority));
  return (bool) $authority_local;
}

/**
 * Validates an authority record.
 *
 * Updates an authority record, setting its "status" to 1.
 *
 * @param $authority
 *   Authority code from Parsian payment.
 * @param $status
 *   TRUE or FALSE.
 */
function _uc_parsian_authority_validate($auhority) {
  db_query("UPDATE {uc_payment_parsian} SET status = 1");
}

/**
 * Removes an orphan incomplete payment authority record from the database.
 *
 * @param $authority
 *   Authority code from Parsian payment.
 */
function _uc_parsian_authority_delete($authority) {
  db_query("DELETE FROM {uc_payment_parsian} WHERE authority = '%s'", $authority);
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
    _uc_parsian_log_error(t('Could not connect to Parsian webservice.'));
  }

  return $soap_client['#return']->call($soap_method, $soap_params);
}

/**
 * Helper function to display and log uc_parsian errors.
 *
 * @param $message
 *   Error message to be logged.
 * @param $redirect
 *   Drupal path to redirect to after error logging.
 *   Set to FALSE to disable redirection.
 */
function _uc_parsian_log_error($message, $redirect = 'cart/checkout') {
  // Display error to the end user.
  drupal_set_message($message, 'error');
  // Log to Watchdog if configured so.
  if (variable_get('uc_parsian_watchdog_log', TRUE)) {
    watchdog('uc_parsian', $message, array(), WATCHDOG_ERROR);
  }
  // Redirect if necessary.
  if ($redirect) {
    drupal_goto($redirect);
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
      return t('@code - Invalid PIN or IP.', array(
        '@code' => $code,
      ));

    case 30:
      return t('@code - The operation has been done previously.', array(
        '@code' => $code,
      ));

    case 34:
      return t('@code - Invalid transaction number.', array(
        '@code' => $code,
      ));

    default:
      return t('@code - Unknown status code. Refer to your technical documentation or contact Parsian bank support.', array(
        '@code' => $code,
      ));
  }
}

